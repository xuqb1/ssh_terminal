/* split_layout.c: SSH dashboard with session management. */
#define _CRT_SECURE_NO_WARNINGS
#define strdup _strdup

#include <cui/cui.h>
#include <cui/cui_app.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#include <libssh/libssh.h>
#include <libssh/sftp.h>

#ifdef _WIN32
#  include <fcntl.h>
#  include <shellapi.h>
#  include <commdlg.h>
#  include <shlobj.h>
#  pragma comment(lib, "comdlg32.lib")
#  pragma comment(lib, "shell32.lib")
#  pragma comment(lib, "ole32.lib")
#  define PATH_SEP '\\'
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/statvfs.h>
#  define PATH_SEP '/'
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#endif

#include "cui_internal.h"
#include "my_terminal.h"

/* ================================================================== */
/* 语言                                                                */
/* ================================================================== */
static char lang = 'z';
#define T(zh, en) (lang == 'z' ? (zh) : (en))

/* ================================================================== */
/* 数据模型                                                            */
/* ================================================================== */

#define MAX_SESSIONS   64
#define MAX_NAME       64
#define MAX_HOST       128
#define MAX_USER       64
#define MAX_PASS       128

#define MAX_FWD_CFG 8

typedef struct {
    bool active;                 /* 是否默认启用 */
    char local_host[64];         /* 默认 127.0.0.1 */
    int  local_port;
    char remote_host[128];
    int  remote_port;
} forward_cfg;

typedef struct {
    char name[MAX_NAME];
    char host[MAX_HOST];
    int  port;
    char user[MAX_USER];
    char pass_enc[MAX_PASS];
    char nav_history[20][512];
    int nav_count;
    forward_cfg fwd[MAX_FWD_CFG];
    int fwd_count; 
    bool connected;
    bool auth_failed;
} ssh_session_cfg;

static ssh_session_cfg g_sessions[MAX_SESSIONS];
static int             g_session_count = 0;
static int             g_last_used[MAX_SESSIONS];
static int             g_use_counter = 0;

static int  g_selected = -1;
static int  g_ctx_target = -1;

static bool g_new_dlg = false;
static bool g_edit_dlg = false;
static bool g_del_dlg = false;
static bool g_retry_dlg = false;
static bool g_log_dlg = false;
static bool g_settings_dlg = false;
static int  g_retry_idx = -1;
static char dlg_name[MAX_NAME], dlg_host[MAX_HOST], dlg_user[MAX_USER], dlg_pass[MAX_PASS];
static int  dlg_port = 22;
static bool dlg_remember = false;

static char g_download_dir[512] = "";
static char g_editor_path[512] = "";
static char g_ssh_key_path[512] = "";        
static char g_settings_dl_buf[512] = "";
static char g_settings_ed_buf[512] = "";
static char g_settings_key_buf[512] = ""; 

/* 统计 */
static float g_cpu = 32.5f, g_ram = 48.2f, g_swap = 12.0f;
static unsigned long long g_ram_used = 14ULL*1024*1024*1024, g_ram_total = 44ULL*1024*1024*1024;
static unsigned long long g_swap_used = 2ULL*1024*1024*1024, g_swap_total = 16ULL*1024*1024*1024;
static float g_load[3] = { 0.22f, 0.09f, 0.03f };
static float g_net_rx = 1.23f, g_net_tx = 0.87f;
#define NET_HIST 20
static float g_rx_hist[NET_HIST], g_tx_hist[NET_HIST];
static int   g_net_head = 0;
static float g_last_refresh = 0;
static unsigned long long g_uptime_sec = 0;

typedef struct { char *name; float mem; float cpu; const char *cmd; } proc_row;
static proc_row g_procs[16] = {
    { NULL, 0, 0, "" }, { NULL, 0, 0, "" }, { NULL, 0, 0, "" }, { NULL, 0, 0, "" },
    { NULL, 0, 0, "" }, { NULL, 0, 0, "" }, { NULL, 0, 0, "" }, { NULL, 0, 0, "" },
    { NULL, 0, 0, "" }, { NULL, 0, 0, "" }, { NULL, 0, 0, "" }, { NULL, 0, 0, "" },
    { NULL, 0, 0, "" }, { NULL, 0, 0, "" }, { NULL, 0, 0, "" }, { NULL, 0, 0, "" },
};
#define PROC_COUNT 16

typedef struct { char path[256]; char used[32]; char total[32]; } dir_usage;
static dir_usage g_dirs[64];
static int g_dir_count = 0;

static char g_cmd[512] = "";
static char g_cmd_log[4096] = "";

typedef struct {
    char name[256];
    char size[32];
    bool is_dir;
    uint64_t bytes;
    char mtime[32];
    char perm[16];
    char owner[64];
} file_row;
static file_row *g_files = NULL;
static int g_file_count = 0, g_file_cap = 0;

/* 持久目录树（递归展开） */
typedef struct {
    char path[512];
    char name[256];
    int  depth;
    bool open;
    bool loaded;   /* 子节点已加载 */
} tree_entry;
static tree_entry g_tree[512];
static int g_tree_count = 0;

static char g_history[20][512];
static int  g_history_count = 0;



static void history_add(const char *p) {
    if (!p || !*p) return;
    for (int i = 0; i < g_history_count; i++)
        if (!strcmp(g_history[i], p)) {
            for (int j = i; j > 0; j--) memcpy(g_history[j], g_history[j-1], sizeof g_history[0]);
            snprintf(g_history[0], sizeof g_history[0], "%s", p);
            return;
        }
    if (g_history_count < 20) {
        for (int j = g_history_count; j > 0; j--) memcpy(g_history[j], g_history[j-1], sizeof g_history[0]);
        g_history_count++;
    } else {
        for (int j = 19; j > 0; j--) memcpy(g_history[j], g_history[j-1], sizeof g_history[0]);
    }
    snprintf(g_history[0], sizeof g_history[0], "%s", p);
}

static cui_table_sort g_proc_sort = { 0, false };
static cui_table_sort g_dir_sort  = { 0, false };
static cui_table_sort g_file_sort = { 0, false };

static int  g_fm_tab   = 0;
static char g_path[512] = "/";
static bool g_history_open = false;
static float g_split_ratio = 1.0f/6.0f;
static float g_v_split = 0.62f;
static float g_left_v_split = 0.30f;
static bool g_dark = false;
static float g_file_split = 0.22f;

/* 文件表选中行 */
static int g_file_selected = -1;
static char g_fm_search[256] = "";

/* 状态栏日志 */
#define LOG_HISTORY 100
static char g_log_history[LOG_HISTORY][512];
static int  g_log_count = 0;
static int  g_log_head = 0;
static char g_status[512] = "";
static double g_status_time = 0;

/* 远程统计采样 */
static float g_stat_last_time = 0;
static unsigned long long g_stat_last_rx_bytes = 0;
static unsigned long long g_stat_last_tx_bytes = 0;
static unsigned long long g_stat_last_cpu_total = 0;
static unsigned long long g_stat_last_cpu_idle = 0;

/* 多实例 tab */
#define MAX_TABS 32
typedef struct {
    int session_idx;
    int inst_id;
    int reconnect_attempts;
    double reconnect_at;
} open_tab;
static open_tab g_tabs[MAX_TABS];
static int g_tab_count = 0;
static int g_next_inst = 1;

static ssh_session    g_tab_ssh[MAX_TABS];
static ssh_channel    g_tab_chan[MAX_TABS];
static sftp_session   g_tab_sftp[MAX_TABS];
static bool           g_tab_shell_ready[MAX_TABS];
static cui_terminal_buffer g_tab_term[MAX_TABS];
static char g_tab_term_text[MAX_TABS][64 * 1024];
static cui_terminal_line g_tab_term_lines[MAX_TABS][4096];

static int g_ssh_tab = 0;
static int g_last_fm_tab = -1;

/* ========== 端口转发（运行时） ========== */
#define MAX_FWD_CONNS 4

typedef struct {
    int          fd;             /* 本地 socket */
    ssh_channel  chan;           /* 远端 forward 通道 */
    bool         chan_open;
    uint64_t     bytes_in, bytes_out;
} forward_conn;

typedef struct {
    bool         running;
    int          tab_idx;
    int          cfg_idx;        /* 对应 g_sessions[sess].fwd[] 的下标 */
    int          sess_idx;       /* 会话索引 */
    int          listen_fd;
    forward_conn conns[MAX_FWD_CONNS];
    int          conn_count;
    char         err[256];
} port_forward;

static port_forward g_forwards[MAX_TABS * MAX_FWD_CFG];
static int g_forward_count = 0;
static bool g_forwards_dlg = false;

/* 双击检测 */
static double g_file_last_click_time[256];
static int    g_file_last_clicked[256];

/* 上传监视 */
#define MAX_UPLOAD_WATCH 16
typedef struct {
    char local_path[1024];
    char remote_path[1024];
    int  tab_idx;
    uint64_t last_mtime;
    uint64_t last_size;
    int retries;
    bool active;
} upload_watch;
static upload_watch g_upload_watch[MAX_UPLOAD_WATCH];
static int g_upload_watch_count = 0;
static float g_last_upload_check = 0;

/* 新建文件 / 新建文件夹 / 属性 弹窗 */
static bool g_newfile_dlg = false;
static bool g_newdir_dlg = false;
static char g_newfile_name[256] = "";
static char g_newdir_name[256] = "";

static bool g_props_dlg = false;
static file_row g_props_row;
static char g_props_path[1024] = "";
static bool g_dir_props_dlg = false;
static char g_dir_props_path[512] = "";
static char g_dir_props_perm[32] = "";
static char g_dir_props_owner[128] = "";
static char g_dir_props_mtime[32] = "";
static int  g_dir_props_files = 0;
static int  g_dir_props_dirs = 0;
static uint64_t g_dir_props_bytes = 0;

/* 剪贴板 */
static char g_clip_path[1024] = "";
static bool g_clip_is_cut = false;
static char g_clip_name[256] = "";

/* 粘贴弹窗 */
static bool g_paste_dlg = false;
static char g_paste_name[256] = "";

static bool g_show_sidebar = true;    /* 左侧栏显示 */
static bool g_show_fm_panel = true;   /* 文件/命令面板显示 */

#define MAX_DIR_QUEUE 4096
static char g_upload_dir_files[MAX_DIR_QUEUE][1024];
static char g_upload_dir_remotes[MAX_DIR_QUEUE][1024];
static int  g_upload_dir_file_count_total = 0;
static int  g_upload_dir_file_count_done = 0;
static int  g_upload_dir_next_idx = 0;
static bool g_upload_dir_active = false;
static char g_upload_dir_name[256] = "";
static char g_upload_dir_remote[1024] = "";

static bool g_rename_dlg = false;
static char g_rename_old[1024] = "";
static char g_rename_new[256] = "";

/* ================================================================== */
/* 加密                                                                */
/* ================================================================== */

static const uint8_t g_key[] = { 0x5A,0x3C,0x91,0xE7,0x42,0x1B,0x88,0xF0,0x6D,0x24 };

static const char b64t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* ========== 分块下载状态机 ========== */
typedef struct {
    bool active;
    int  transfer_idx;    /* 对应 g_transfers[] 的下标 */
    int  tab_idx;
    char remote_path[512];
    char local_path[1024];
    sftp_file file;
    FILE *out;
    uint64_t total_size;
    uint64_t done_size;
    char err[256];
    bool open_after;      /* 下载完成后用编辑器打开 */
    char file_name[256];  /* 原始文件名（用于打开） */
} download_task;

static download_task g_dl = { 0 };

typedef struct {
    bool active;
    int  transfer_idx;
    int  tab_idx;
    char local_path[1024];
    char remote_path[1024];
    FILE *in;
    sftp_file file;
    uint64_t total_size;
    uint64_t done_size;
    char err[256];
} upload_task;

static upload_task g_ul = { 0 };

#define MAX_TRANSFERS 64
typedef enum { XFER_DOWNLOAD = 0, XFER_UPLOAD, XFER_UPLOAD_DIR } xfer_type;
typedef enum { XFER_PENDING = 0, XFER_ACTIVE, XFER_DONE, XFER_FAILED } xfer_status;

typedef struct {
    xfer_type   type;
    xfer_status status;
    char        local[1024];
    char        remote[1024];
    uint64_t    total;
    uint64_t    done;
    int         tab_idx;
    time_t      start_time;
    char        err[256];
} transfer_entry;

static transfer_entry g_transfers[MAX_TRANSFERS];
static int g_transfer_count = 0;
static bool g_transfers_dlg = false;

/* 手画右键菜单 */
enum {
    CTX_ACTION_NONE = 0,
    CTX_ACTION_REFRESH, 
    CTX_ACTION_NEW_FILE,
    CTX_ACTION_NEW_DIR,
    CTX_ACTION_COPY,
    CTX_ACTION_CUT,
    CTX_ACTION_PASTE,
    CTX_ACTION_DOWNLOAD,
    CTX_ACTION_UPLOAD,
    CTX_ACTION_DELETE,
    CTX_ACTION_RENAME, 
    CTX_ACTION_PROPS,
    CTX_ACTION_SEPARATOR,
};

static bool g_fctx_open = false;
static int  g_fctx_target = -1;
static float g_fctx_x = 0, g_fctx_y = 0;
static char g_fctx_items[16][64];
static int  g_fctx_item_count = 0;
static int  g_fctx_item_action[16];
static int  g_fctx_hover = -1;

static bool g_about_dlg = false;

static bool g_history_dlg = false;
#define MAX_CMDS 256
typedef struct {
    char name[128];
    char command[4096];    /* 多行命令，可含 \n */
    time_t created;
    bool add_cr;           /* 末尾添加回车符 CR */
} cmd_entry;

static cmd_entry g_cmds[MAX_CMDS];
static int g_cmd_count = 0;

/* 命令列表持久化 */
static char g_cmds_file[512] = "";

static bool g_cmd_add_dlg = false;
static char g_cmd_add_name[128] = "";
static char g_cmd_add_body[4096] = "";
static bool g_cmd_add_cr = true;
static int g_cmd_edit_idx = -1;   /* -1 = 添加，>= 0 = 编辑 */


/* ========== Linux 常用指令 ========== */

typedef struct {
    char category_zh[64], category_en[64];
    char name_zh[128],    name_en[128];
    char desc_zh[256],    desc_en[256];
    char body[1024];
    char hint_zh[5][128], hint_en[5][128];
    char example[1024];
    bool add_cr;
    bool builtin;
} linux_cmd;

#define MAX_LINUX_CMDS 256
static linux_cmd *g_linux_cmds = NULL;
static int g_linux_cmd_count = 0;
static int g_linux_cmd_cap = 0;
static char g_linux_filter[128] = "";
static int g_linux_cat_filter = 0;   /* 0 = 全部，>0 = 选中第几个分类 */
static cui_table_sort g_linux_sort = { -1, false };
static bool g_linux_run_dlg = false;
static int  g_linux_run_idx = -1;
static char g_linux_args[5][256];
static char g_linux_preview[2048];

static bool g_linux_add_dlg = false;
static bool g_linux_edit_dlg = false;
static int  g_linux_edit_idx = -1;
static bool g_linux_del_dlg = false;
static int  g_linux_del_idx = -1;
static linux_cmd g_linux_edit_buf;

/* 参数提示：空串表示无此参数 */
static const linux_cmd g_linux_builtin[] = {
    /* ---- 文件与磁盘 ---- */
    { "文件与磁盘", "Files & disks", "查看磁盘使用", "Disk usage",
      "显示各挂载点的磁盘用量", "Show disk usage by mount point",
      "df -h {1}",
      { "挂载点，例如 / 或 /data；留空显示全部", "", "", "", "" },
      { "Mount point, e.g. / or /data; empty for all", "", "", "", "" },
      "df -h /", true, true },
    { "文件与磁盘", "Files & disks", "查看目录大小", "Folder size",
      "统计目录下每个子项的大小", "Size of each item in a folder",
      "du -sh {1}/*",
      { "目录，例如 /var/log；用 . 表示当前", "", "", "", "" },
      { "Folder, e.g. /var/log; use . for current", "", "", "", "" },
      "du -sh /var/log/*", true, true },
    { "文件与磁盘", "Files & disks", "查找大文件", "Find large files",
      "查找大于指定大小的文件", "Find files larger than a size",
      "find {1} -type f -size +{2} -exec ls -lh {} \\;",
      { "起始目录，例如 /var", "大小，例如 100M 或 1G", "", "", "" },
      { "Start folder, e.g. /var", "Size, e.g. 100M or 1G", "", "", "" },
      "find /var -type f -size +100M -exec ls -lh {} \\;", true, true },
    { "文件与磁盘", "Files & disks", "查找文件", "Find files",
      "按文件名查找", "Find files by name",
      "find {1} -name \"{2}\"",
      { "起始目录，例如 /etc", "文件名模式，例如 *.conf", "", "", "" },
      { "Start folder, e.g. /etc", "Name pattern, e.g. *.conf", "", "", "" },
      "find /etc -name \"*.conf\"", true, true },
    { "文件与磁盘", "Files & disks", "解压 tar.gz", "Extract tar.gz",
      "解压到指定目录", "Extract into a folder",
      "tar -xzvf {1} -C {2}",
      { "压缩包路径，例如 app.tar.gz", "目标目录，例如 /opt", "", "", "" },
      { "Archive, e.g. app.tar.gz", "Target folder, e.g. /opt", "", "", "" },
      "tar -xzvf app.tar.gz -C /opt", true, true },
    { "文件与磁盘", "Files & disks", "打包 tar.gz", "Create tar.gz",
      "把目录打包成 tar.gz", "Pack a folder into tar.gz",
      "tar -czvf {1}.tar.gz {1}",
      { "目录名，例如 myapp", "", "", "", "" },
      { "Folder name, e.g. myapp", "", "", "", "" },
      "tar -czvf myapp.tar.gz myapp", true, true },
    { "文件与磁盘", "Files & disks", "修改权限", "chmod",
      "修改文件或目录权限", "Change file or folder permissions",
      "chmod {1} {2}",
      { "权限，例如 755 或 644", "路径，例如 /data/app.sh", "", "", "" },
      { "Mode, e.g. 755 or 644", "Path, e.g. /data/app.sh", "", "", "" },
      "chmod 755 /data/app.sh", true, true },
    { "文件与磁盘", "Files & disks", "修改所有者", "chown",
      "修改文件或目录所有者", "Change file or folder owner",
      "chown {1}:{1} {2}",
      { "用户/组，例如 root", "路径，例如 /data/app", "", "", "" },
      { "User/group, e.g. root", "Path, e.g. /data/app", "", "", "" },
      "chown root:root /data/app", true, true },

    /* ---- 进程与服务 ---- */
    { "进程与服务", "Processes & services", "按内存排序进程", "Processes by memory",
      "列出占用内存最多的进程", "Processes using the most memory",
      "ps -eo pid,comm,%cpu,%mem --sort=-%mem | head -n {1}",
      { "显示条数，例如 10", "", "", "", "" },
      { "How many rows, e.g. 10", "", "", "", "" },
      "ps -eo pid,comm,%cpu,%mem --sort=-%mem | head -n 10", true, true },
    { "进程与服务", "Processes & services", "查看端口占用（PID）", "Port owner (PID)",
      "查看监听某端口的进程 PID", "PID listening on a port",
      "ss -tlnp | grep ':{1} '",
      { "端口号，例如 22 或 8080", "", "", "", "" },
      { "Port, e.g. 22 or 8080", "", "", "", "" },
      "ss -tlnp | grep ':22 '", true, true },
    { "进程与服务", "Processes & services", "查看端口占用（lsof）", "Port owner (lsof)",
      "用 lsof 查看某端口的进程", "Find the process on a port with lsof",
      "lsof -i :{1}",
      { "端口号，例如 3306", "", "", "", "" },
      { "Port, e.g. 3306", "", "", "", "" },
      "lsof -i :3306", true, true },
    { "进程与服务", "Processes & services", "杀进程（TERM）", "Kill by PID (TERM)",
      "向指定 PID 发送 TERM 信号", "Send TERM to a PID",
      "kill -15 {1}",
      { "进程 PID，例如 12345", "", "", "", "" },
      { "Process PID, e.g. 12345", "", "", "", "" },
      "kill -15 12345", true, true },
    { "进程与服务", "Processes & services", "强制杀进程（KILL）", "Force kill (KILL)",
      "向指定 PID 发送 KILL 信号", "Send KILL to a PID",
      "kill -9 {1}",
      { "进程 PID，例如 12345", "", "", "", "" },
      { "Process PID, e.g. 12345", "", "", "", "" },
      "kill -9 12345", true, true },
    { "进程与服务", "Processes & services", "按名称杀进程", "Kill by name",
      "结束所有名称匹配的进程", "Kill every process matching a name",
      "pkill -f {1}",
      { "进程名或命令行片段，例如 nginx", "", "", "", "" },
      { "Process name or fragment, e.g. nginx", "", "", "", "" },
      "pkill -f nginx", true, true },
    { "进程与服务", "Processes & services", "查看服务状态", "Service status",
      "systemd 服务状态", "systemd unit status",
      "systemctl status {1}",
      { "服务名，例如 nginx", "", "", "", "" },
      { "Unit name, e.g. nginx", "", "", "", "" },
      "systemctl status nginx", true, true },
    { "进程与服务", "Processes & services", "启动服务", "Start service",
      "systemd 启动服务", "Start a systemd unit",
      "systemctl start {1}",
      { "服务名，例如 nginx", "", "", "", "" },
      { "Unit name, e.g. nginx", "", "", "", "" },
      "systemctl start nginx", true, true },
    { "进程与服务", "Processes & services", "停止服务", "Stop service",
      "systemd 停止服务", "Stop a systemd unit",
      "systemctl stop {1}",
      { "服务名，例如 nginx", "", "", "", "" },
      { "Unit name, e.g. nginx", "", "", "", "" },
      "systemctl stop nginx", true, true },
    { "进程与服务", "Processes & services", "重启服务", "Restart service",
      "systemd 重启服务", "Restart a systemd unit",
      "systemctl restart {1}",
      { "服务名，例如 nginx", "", "", "", "" },
      { "Unit name, e.g. nginx", "", "", "", "" },
      "systemctl restart nginx", true, true },
    { "进程与服务", "Processes & services", "列出运行中的服务", "Running services",
      "所有正在运行的服务", "All running services",
      "systemctl list-units --type=service --state=running",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "systemctl list-units --type=service --state=running", true, true },
    { "进程与服务", "Processes & services", "列出失败的服务", "Failed services",
      "所有失败的服务", "All failed services",
      "systemctl --failed",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "systemctl --failed", true, true },
    { "进程与服务", "Processes & services", "开机自启", "Enable service",
      "设置服务开机自启", "Enable a unit at boot",
      "systemctl enable {1}",
      { "服务名，例如 nginx", "", "", "", "" },
      { "Unit name, e.g. nginx", "", "", "", "" },
      "systemctl enable nginx", true, true },
    { "进程与服务", "Processes & services", "关闭开机自启", "Disable service",
      "取消服务开机自启", "Disable a unit at boot",
      "systemctl disable {1}",
      { "服务名，例如 nginx", "", "", "", "" },
      { "Unit name, e.g. nginx", "", "", "", "" },
      "systemctl disable nginx", true, true },
    { "进程与服务", "Processes & services", "查看服务日志", "Service log",
      "实时跟踪服务日志", "Follow a unit's log",
      "journalctl -u {1} -f",
      { "服务名，例如 nginx", "", "", "", "" },
      { "Unit name, e.g. nginx", "", "", "", "" },
      "journalctl -u nginx -f", true, true },

    /* ---- Java ---- */
    { "Java", "Java", "查看所有 java 进程", "All Java processes",
      "列出所有 java 进程的 PID 和命令", "PID and command of every Java process",
      "ps -ef | grep [j]ava",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "ps -ef | grep [j]ava", true, true },
    { "Java", "Java", "查看 java 进程（jps）", "Java processes (jps)",
      "用 jps 列出 java 进程", "List Java processes with jps",
      "jps -l",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "jps -l", true, true },
    { "Java", "Java", "查看 java 版本", "Java version",
      "显示当前 java 版本", "Show the java version",
      "java -version",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "java -version", true, true },
    { "Java", "Java", "查看 java 线程栈", "Java thread dump",
      "打印指定 java 进程的线程栈", "Thread dump of a Java process",
      "jstack {1}",
      { "java 进程 PID", "", "", "", "" },
      { "Java process PID", "", "", "", "" },
      "jstack 12345", true, true },
    { "Java", "Java", "查看 java 堆", "Java heap",
      "显示 java 进程的堆信息", "Heap info of a Java process",
      "jmap -heap {1}",
      { "java 进程 PID", "", "", "", "" },
      { "Java process PID", "", "", "", "" },
      "jmap -heap 12345", true, true },

    /* ---- 网络 ---- */
    { "网络", "Network", "网络连通测试", "Ping",
      "ping 指定主机", "Ping a host",
      "ping -c 4 {1}",
      { "主机或 IP，例如 8.8.8.8", "", "", "", "" },
      { "Host or IP, e.g. 8.8.8.8", "", "", "", "" },
      "ping -c 4 8.8.8.8", true, true },
    { "网络", "Network", "查看路由", "Routes",
      "显示路由表", "Show the routing table",
      "ip route",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "ip route", true, true },
    { "网络", "Network", "查看 IP", "IP addresses",
      "显示所有网卡的 IP", "IPs of every interface",
      "ip -br addr",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "ip -br addr", true, true },

    /* ---- 系统信息 ---- */
    { "系统信息", "System", "查看内存", "Memory",
      "显示内存和 Swap 使用", "RAM and swap usage",
      "free -h",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "free -h", true, true },
    { "系统信息", "System", "查看系统版本", "OS release",
      "显示发行版信息", "Distribution release",
      "cat /etc/os-release",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "cat /etc/os-release", true, true },
    { "系统信息", "System", "查看内核版本", "Kernel",
      "显示内核版本", "Kernel version",
      "uname -a",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "uname -a", true, true },
    { "系统信息", "System", "查看登录用户", "Logged-in users",
      "当前登录的用户", "Who is logged in",
      "who",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "who", true, true },
    { "系统信息", "System", "查看定时任务", "Crontab",
      "当前用户的 crontab", "Current user's crontab",
      "crontab -l",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "crontab -l", true, true },
    { "系统信息", "System", "查看环境变量", "Environment",
      "显示所有环境变量", "All environment variables",
      "env | sort",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "env | sort", true, true },
    /* ---- nano ---- */
    { "nano", "nano", "用 nano 编辑文件", "Edit a file with nano",
      "用 nano 打开或新建文件；保存 Ctrl+O，退出 Ctrl+X",
      "Open or create a file with nano; Ctrl+O saves, Ctrl+X exits",
      "nano {1}",
      { "文件路径，例如 /etc/nginx/nginx.conf", "", "", "", "" },
      { "File path, e.g. /etc/nginx/nginx.conf", "", "", "", "" },
      "nano /etc/nginx/nginx.conf", true, true },
    { "nano", "nano", "用 nano 只读打开", "Open read-only with nano",
      "只读方式打开，防止误改",
      "Open read-only to avoid accidental edits",
      "nano -v {1}",
      { "文件路径", "", "", "", "" },
      { "File path", "", "", "", "" },
      "nano -v /etc/passwd", true, true },

    /* ---- vi/vim ---- */
    { "vi/vim", "vi/vim", "用 vim 编辑文件", "Edit a file with vim",
      "用 vim 打开文件；:w 保存，:q 退出，:wq 保存退出",
      "Open a file with vim; :w saves, :q quits, :wq saves and quits",
      "vim {1}",
      { "文件路径，例如 /etc/hosts", "", "", "", "" },
      { "File path, e.g. /etc/hosts", "", "", "", "" },
      "vim /etc/hosts", true, true },
    { "vi/vim", "vi/vim", "用 vim 只读打开", "Open read-only with vim",
      "只读方式打开，防止误改",
      "Open read-only to avoid accidental edits",
      "vim -R {1}",
      { "文件路径", "", "", "", "" },
      { "File path", "", "", "", "" },
      "vim -R /etc/passwd", true, true },
    { "vi/vim", "vi/vim", "用 vim 打开并跳到行", "Open vim at a line",
      "打开文件并跳到指定行",
      "Open a file and jump to a line",
      "vim +{1} {2}",
      { "行号，例如 120", "文件路径", "", "", "" },
      { "Line number, e.g. 120", "File path", "", "", "" },
      "vim +120 /var/log/syslog", true, true },

    /* ---- 文件与磁盘（补充） ---- */
    { "文件与磁盘", "Files & disks", "解压 zip", "Extract zip",
      "解压 zip 到指定目录；需要安装 unzip",
      "Extract a zip into a folder; requires unzip",
      "unzip {1} -d {2}",
      { "压缩包路径，例如 app.zip", "目标目录，例如 /opt", "", "", "" },
      { "Archive, e.g. app.zip", "Target folder, e.g. /opt", "", "", "" },
      "unzip app.zip -d /opt", true, true },
    { "文件与磁盘", "Files & disks", "创建 zip", "Create zip",
      "把目录打包成 zip；需要安装 zip",
      "Pack a folder into a zip; requires zip",
      "zip -r {1}.zip {1}",
      { "目录名，例如 myapp", "", "", "", "" },
      { "Folder name, e.g. myapp", "", "", "", "" },
      "zip -r myapp.zip myapp", true, true },
    { "文件与磁盘", "Files & disks", "解压 tar.bz2", "Extract tar.bz2",
      "解压 tar.bz2 到当前目录",
      "Extract a tar.bz2 into the current folder",
      "tar -xjvf {1}",
      { "压缩包路径，例如 app.tar.bz2", "", "", "", "" },
      { "Archive, e.g. app.tar.bz2", "", "", "", "" },
      "tar -xjvf app.tar.bz2", true, true },
    { "文件与磁盘", "Files & disks", "解压 tar.xz", "Extract tar.xz",
      "解压 tar.xz 到当前目录",
      "Extract a tar.xz into the current folder",
      "tar -xJvf {1}",
      { "压缩包路径，例如 app.tar.xz", "", "", "", "" },
      { "Archive, e.g. app.tar.xz", "", "", "", "" },
      "tar -xJvf app.tar.xz", true, true },
    { "文件与磁盘", "Files & disks", "查看文件类型", "File type",
      "显示文件的真实类型",
      "Show the real type of a file",
      "file {1}",
      { "文件路径，例如 /bin/ls", "", "", "", "" },
      { "File path, e.g. /bin/ls", "", "", "", "" },
      "file /bin/ls", true, true },
    { "文件与磁盘", "Files & disks", "查看文件大小", "File size",
      "以人类可读格式显示文件信息",
      "Show file info in human-readable form",
      "ls -lh {1}",
      { "文件或目录路径", "", "", "", "" },
      { "File or folder path", "", "", "", "" },
      "ls -lh /var/log/syslog", true, true },
    { "文件与磁盘", "Files & disks", "查看目录树", "Directory tree",
      "树状显示目录结构；需要安装 tree",
      "Show a folder as a tree; requires tree",
      "tree -L {1} {2}",
      { "层级，例如 2", "目录，例如 /etc", "", "", "" },
      { "Depth, e.g. 2", "Folder, e.g. /etc", "", "", "" },
      "tree -L 2 /etc", true, true },
    { "文件与磁盘", "Files & disks", "查找最近修改的文件", "Recently modified files",
      "查找 N 天内修改过的文件",
      "Find files modified within N days",
      "find {1} -type f -mtime -{2}",
      { "起始目录，例如 /var", "天数，例如 7", "", "", "" },
      { "Start folder, e.g. /var", "Days, e.g. 7", "", "", "" },
      "find /var -type f -mtime -7", true, true },
    { "文件与磁盘", "Files & disks", "同步目录（本地）", "Sync folders (local)",
      "用 rsync 同步两个本地目录；需要安装 rsync",
      "Sync two local folders with rsync; requires rsync",
      "rsync -av --delete {1}/ {2}/",
      { "源目录，例如 /data/src", "目标目录，例如 /backup/src", "", "", "" },
      { "Source, e.g. /data/src", "Target, e.g. /backup/src", "", "", "" },
      "rsync -av --delete /data/src/ /backup/src/", true, true },

    /* ---- 文本处理 ---- */
    { "文本处理", "Text", "查看文件前 N 行", "First N lines",
      "显示文件开头若干行",
      "Show the first N lines of a file",
      "head -n {1} {2}",
      { "行数，例如 20", "文件路径", "", "", "" },
      { "Lines, e.g. 20", "File path", "", "", "" },
      "head -n 20 /var/log/syslog", true, true },
    { "文本处理", "Text", "查看文件后 N 行", "Last N lines",
      "显示文件末尾若干行",
      "Show the last N lines of a file",
      "tail -n {1} {2}",
      { "行数，例如 50", "文件路径", "", "", "" },
      { "Lines, e.g. 50", "File path", "", "", "" },
      "tail -n 50 /var/log/syslog", true, true },
    { "文本处理", "Text", "统计文件行数", "Count lines",
      "统计文件的行数、词数、字节数",
      "Lines, words and bytes of a file",
      "wc -l {1}",
      { "文件路径", "", "", "", "" },
      { "File path", "", "", "", "" },
      "wc -l /etc/passwd", true, true },
    { "文本处理", "Text", "搜索文件内容", "Grep in a file",
      "在文件中搜索关键词，显示行号",
      "Search a file for a keyword, with line numbers",
      "grep -n {1} {2}",
      { "关键词，例如 error", "文件路径", "", "", "" },
      { "Keyword, e.g. error", "File path", "", "", "" },
      "grep -n error /var/log/syslog", true, true },
    { "文本处理", "Text", "递归搜索目录内容", "Grep recursively",
      "在目录下递归搜索关键词",
      "Search a folder recursively for a keyword",
      "grep -rn {1} {2}",
      { "关键词，例如 TODO", "目录，例如 /data/app", "", "", "" },
      { "Keyword, e.g. TODO", "Folder, e.g. /data/app", "", "", "" },
      "grep -rn TODO /data/app", true, true },
    { "文本处理", "Text", "按列提取", "Extract a column",
      "用 awk 提取第 N 列",
      "Extract the Nth column with awk",
      "awk '{print ${1}}' {2}",
      { "列号，例如 1 或 3", "文件路径", "", "", "" },
      { "Column, e.g. 1 or 3", "File path", "", "", "" },
      "awk '{print $1}' /etc/passwd", true, true },
    { "文本处理", "Text", "替换文本", "Replace text",
      "用 sed 替换文件中的文本（原地修改）",
      "Replace text in a file in place with sed",
      "sed -i 's/{1}/{2}/g' {3}",
      { "原文本，例如 old", "新文本，例如 new", "文件路径", "", "" },
      { "Old text, e.g. old", "New text, e.g. new", "File path", "", "" },
      "sed -i 's/old/new/g' /etc/my.conf", true, true },
    { "文本处理", "Text", "排序去重", "Sort and unique",
      "排序并去掉重复行",
      "Sort and remove duplicate lines",
      "sort {1} | uniq",
      { "文件路径", "", "", "", "" },
      { "File path", "", "", "", "" },
      "sort /etc/passwd | uniq", true, true },
    { "文本处理", "Text", "比较两个文件", "Diff two files",
      "逐行比较两个文件的差异",
      "Compare two files line by line",
      "diff -u {1} {2}",
      { "文件 1", "文件 2", "", "", "" },
      { "File 1", "File 2", "", "", "" },
      "diff -u /etc/hosts /etc/hosts.bak", true, true },

    /* ---- 进程与服务（补充） ---- */
    { "进程与服务", "Processes & services", "查看进程树", "Process tree",
      "树状显示进程父子关系",
      "Show processes as a tree",
      "ps -ef --forest",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "ps -ef --forest", true, true },
    { "进程与服务", "Processes & services", "实时资源快照", "Resource snapshot",
      "一次性输出系统资源快照",
      "One-shot snapshot of system resources",
      "vmstat 1 5",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "vmstat 1 5", true, true },
    { "进程与服务", "Processes & services", "查看进程打开的文件", "Open files of a process",
      "列出进程打开的文件；需要安装 lsof",
      "List files opened by a process; requires lsof",
      "lsof -p {1}",
      { "进程 PID", "", "", "", "" },
      { "Process PID", "", "", "", "" },
      "lsof -p 12345", true, true },
    { "进程与服务", "Processes & services", "查看进程启动命令", "Process command line",
      "显示进程的完整启动命令",
      "Show the full command line of a process",
      "ps -p {1} -o pid,cmd",
      { "进程 PID", "", "", "", "" },
      { "Process PID", "", "", "", "" },
      "ps -p 12345 -o pid,cmd", true, true },

    /* ---- 网络（补充） ---- */
    { "网络", "Network", "下载文件", "Download a file",
      "用 curl 下载到当前目录，保留原文件名",
      "Download to the current folder with curl, keeping the name",
      "curl -O {1}",
      { "URL，例如 https://example.com/file.tar.gz", "", "", "", "" },
      { "URL, e.g. https://example.com/file.tar.gz", "", "", "", "" },
      "curl -O https://example.com/file.tar.gz", true, true },
    { "网络", "Network", "查看 HTTP 响应", "HTTP response",
      "显示响应头和正文",
      "Show response headers and body",
      "curl -i {1}",
      { "URL，例如 https://example.com", "", "", "", "" },
      { "URL, e.g. https://example.com", "", "", "", "" },
      "curl -i https://example.com", true, true },
    { "网络", "Network", "测试端口连通", "Test a TCP port",
      "测试主机端口是否开放；需要安装 nc",
      "Test whether a host port is open; requires nc",
      "nc -zv {1} {2}",
      { "主机或 IP，例如 10.0.0.5", "端口，例如 3306", "", "", "" },
      { "Host or IP, e.g. 10.0.0.5", "Port, e.g. 3306", "", "", "" },
      "nc -zv 10.0.0.5 3306", true, true },
    { "网络", "Network", "DNS 查询", "DNS lookup",
      "查询域名的 DNS 记录；需要安装 dig",
      "Query DNS records; requires dig",
      "dig {1}",
      { "域名，例如 example.com", "", "", "", "" },
      { "Domain, e.g. example.com", "", "", "", "" },
      "dig example.com", true, true },
    { "网络", "Network", "查看连接状态", "Connection state",
      "显示所有 TCP 连接及状态",
      "Show all TCP connections and their state",
      "ss -tan",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "ss -tan", true, true },

    /* ---- 系统信息（补充） ---- */
    { "系统信息", "System", "查看 CPU 信息", "CPU info",
      "显示 CPU 型号、核心数等",
      "Show CPU model and core count",
      "lscpu",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "lscpu", true, true },
    { "系统信息", "System", "查看块设备", "Block devices",
      "显示磁盘和分区",
      "Show disks and partitions",
      "lsblk",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "lsblk", true, true },
    { "系统信息", "System", "查看 PCI 设备", "PCI devices",
      "列出 PCI 设备",
      "List PCI devices",
      "lspci",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "lspci", true, true },
    { "系统信息", "System", "查看已安装包（Debian/Ubuntu）", "Installed packages (Debian/Ubuntu)",
      "列出已安装的 deb 包",
      "List installed deb packages",
      "dpkg -l | grep {1}",
      { "包名片段，例如 nginx", "", "", "", "" },
      { "Package fragment, e.g. nginx", "", "", "", "" },
      "dpkg -l | grep nginx", true, true },
    { "系统信息", "System", "查看已安装包（RHEL/CentOS）", "Installed packages (RHEL/CentOS)",
      "列出已安装的 rpm 包",
      "List installed rpm packages",
      "rpm -qa | grep {1}",
      { "包名片段，例如 nginx", "", "", "", "" },
      { "Package fragment, e.g. nginx", "", "", "", "" },
      "rpm -qa | grep nginx", true, true },
    { "系统信息", "System", "查看系统启动时间", "Boot time",
      "显示系统上次启动的时间",
      "Show when the system last booted",
      "who -b",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "who -b", true, true },
    { "系统信息", "System", "查看系统负载", "Load average",
      "显示 1/5/15 分钟负载和运行时间",
      "1/5/15-minute load and uptime",
      "uptime",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "uptime", true, true },
    { "系统信息", "System", "查看磁盘 IO", "Disk I/O",
      "显示各磁盘的 IO 统计；需要安装 sysstat",
      "I/O stats per disk; requires sysstat",
      "iostat -x 1 3",
      { "", "", "", "", "" }, { "", "", "", "", "" },
      "iostat -x 1 3", true, true },
};
#define LINUX_BUILTIN_COUNT ((int)(sizeof g_linux_builtin / sizeof g_linux_builtin[0]))

/* 取字段的宏，按语言 */
#define LCMD_CAT(c)   (lang == 'z' ? (c)->category_zh : (c)->category_en)
#define LCMD_NAME(c)  (lang == 'z' ? (c)->name_zh    : (c)->name_en)
#define LCMD_DESC(c)  (lang == 'z' ? (c)->desc_zh    : (c)->desc_en)
#define LCMD_HINT(c,k)(lang == 'z' ? (c)->hint_zh[k] : (c)->hint_en[k])



static float g_term_font_size = 13.0f;
static char g_settings_term_size_buf[16] = "";

#ifdef _WIN32
#  define strcasecmp _stricmp
#endif
/* 文本文件判断 */
static char g_text_noext[512] = "Makefile,makefile,Dockerfile,CMakeLists.txt,README,LICENSE,CHANGELOG,.gitignore,.env";
static char g_text_exts[1024] = "txt,md,markdown,c,h,cpp,hpp,cc,cxx,py,js,ts,jsx,tsx,json,xml,yaml,yml,html,htm,css,scss,less,sh,bash,zsh,ini,conf,cfg,toml,log,csv,tsv,sql,java,go,rs,rb,php,pl,lua,r,m,swift,kt,scala,cs,fs,vb,pas,asm,cmake,gradle,properties,env,tex,rst,adoc,org";
static char g_settings_noext_buf[512] = "";
static char g_settings_exts_buf[1024] = "";

//static bool g_dark = false;
//static float g_file_split = 0.22f;
static int g_win_w = 1440;          /* ← 新增 */
static int g_win_h = 900;           /* ← 新增 */
//static float g_term_font_size = 13.0f;   /* ← 从命令面板段挪上来 */
static int g_cmd_selected = -1;

static void commands_panel(cui_ctx *ui);
static bool exec_remote(ssh_session s, const char *cmd, char *out, size_t cap);
static void hline(cui_ctx *ui);
static void scan_remote(const char *path);
static void format_perm(mode_t perms, bool is_dir, char *out, size_t cap);
static void start_forward(int tab_idx, int sess_idx, int cfg_idx);
static void stop_forward(int idx);
static void poll_forwards(cui_ctx *ui);

static void sortable_header(cui_ctx *ui, const char *table, const char *label, float width,
                            bool grow, cui_text_align align, cui_table_sort *sort, int column);
static bool str_contains_ci(const char *haystack, const char *needle);

static void b64enc(const uint8_t *in, int n, char *out) {
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i+1 < n) v |= (uint32_t)in[i+1] << 8;
        if (i+2 < n) v |= in[i+2];
        out[o++] = b64t[(v >> 18) & 63];
        out[o++] = b64t[(v >> 12) & 63];
        out[o++] = (i+1 < n) ? b64t[(v >> 6) & 63] : '=';
        out[o++] = (i+2 < n) ? b64t[v & 63]        : '=';
    }
    out[o] = 0;
}
static int b64dec(const char *in, uint8_t *out) {
    int o = 0;
    uint32_t v = 0; int bits = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '=') break;
        const char *q = strchr(b64t, *p);
        if (!q) continue;
        v = (v << 6) | (uint32_t)(q - b64t);
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (v >> bits) & 0xFF; }
    }
    return o;
}
static void encrypt_pass(const char *plain, char *out) {
    int n = (int)strlen(plain);
    uint8_t *buf = (uint8_t*)malloc(n > 0 ? n : 1);
    for (int i = 0; i < n; i++) buf[i] = (uint8_t)plain[i] ^ g_key[i % sizeof g_key];
    b64enc(buf, n, out);
    free(buf);
}
static void decrypt_pass(const char *enc, char *out) {
    uint8_t buf[MAX_PASS];
    int n = b64dec(enc, buf);
    for (int i = 0; i < n; i++) out[i] = (char)(buf[i] ^ g_key[i % sizeof g_key]);
    out[n] = 0;
}
static bool same_dir(const char *path1, const char *path2) {
    char d1[512], d2[512];
    snprintf(d1, sizeof d1, "%s", path1);
    snprintf(d2, sizeof d2, "%s", path2);
    char *s1 = strrchr(d1, '/');
    char *s2 = strrchr(d2, '/');
    if (s1) *s1 = 0;
    if (s2) *s2 = 0;
    return !strcmp(d1, d2);
}

/* ================================================================== */
/* 状态栏                                                              */
/* ================================================================== */

static void log_status(const char *fmt, ...) {
    char newline[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(newline, sizeof newline, fmt, ap);
    va_end(ap);
    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);
    char ts[32] = "";
    if (tmv) strftime(ts, sizeof ts, "%H:%M:%S", tmv);
    char with_ts[600];
    snprintf(with_ts, sizeof with_ts, "[%s] %s", ts, newline);
    int idx = g_log_head;
    snprintf(g_log_history[idx], sizeof g_log_history[idx], "%s", with_ts);
    g_log_head = (g_log_head + 1) % LOG_HISTORY;
    if (g_log_count < LOG_HISTORY) g_log_count++;
    snprintf(g_status, sizeof g_status, "%s", with_ts);
    g_status_time = 0;
}

/* 状态栏 + Toast 同时输出。tone 决定 Toast 颜色。
 * fmt 应已用 T() 翻译，例如：
 *   log_status_toast(ui, CUI_TONE_SUCCESS, T("已连接 %s", "Connected %s"), host);
 */
static void log_status_toast(cui_ctx *ui, cui_tone tone, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    /* 状态栏：复用 log_status 的时间戳和环形日志 */
    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);
    char ts[32] = "";
    if (tmv) strftime(ts, sizeof ts, "%H:%M:%S", tmv);
    char with_ts[600];
    snprintf(with_ts, sizeof with_ts, "[%s] %s", ts, msg);

    int idx = g_log_head;
    snprintf(g_log_history[idx], sizeof g_log_history[idx], "%s", with_ts);
    g_log_head = (g_log_head + 1) % LOG_HISTORY;
    if (g_log_count < LOG_HISTORY) g_log_count++;
    snprintf(g_status, sizeof g_status, "%s", with_ts);
    g_status_time = 0;

    /* Toast */
    if (ui) {
        cui_toast(ui, msg, .tone = tone, .duration = 2.5f);
    }
}

static void ini_escape(FILE *f, const char *s) {
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '=':  fputs("[eq]", f); break;
        case '\n': fputs("[nl]", f); break;
        case '\r': fputs("[cr]", f); break;
        case '[':  fputs("[lb]", f); break;
        case ']':  fputs("[rb]", f); break;
        case '\\': fputs("[bs]", f); break;
        default:   fputc(*p, f); break;
        }
    }
}

static void ini_unescape(const char *s, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = s; *p && n < cap - 1; ) {
        if (p[0] == '[' && p[1] == 'e' && p[2] == 'q' && p[3] == ']') { out[n++] = '=';  p += 4; }
        else if (p[0] == '[' && p[1] == 'n' && p[2] == 'l' && p[3] == ']') { out[n++] = '\n'; p += 4; }
        else if (p[0] == '[' && p[1] == 'c' && p[2] == 'r' && p[3] == ']') { out[n++] = '\r'; p += 4; }
        else if (p[0] == '[' && p[1] == 'l' && p[2] == 'b' && p[3] == ']') { out[n++] = '[';  p += 4; }
        else if (p[0] == '[' && p[1] == 'r' && p[2] == 'b' && p[3] == ']') { out[n++] = ']';  p += 4; }
        else if (p[0] == '[' && p[1] == 'b' && p[2] == 's' && p[3] == ']') { out[n++] = '\\'; p += 4; }
        else { out[n++] = *p++; }
    }
    out[n] = 0;
}

/* ================================================================== */
/* 默认下载目录                                                        */
/* ================================================================== */

static void default_download_dir(char *out, size_t cap) {
#ifdef _WIN32
    const char *profile = getenv("USERPROFILE");
    if (profile) snprintf(out, cap, "%s\\Downloads", profile);
    else snprintf(out, cap, "C:\\Downloads");
#else
    const char *home = getenv("HOME");
    if (home) snprintf(out, cap, "%s/Downloads", home);
    else snprintf(out, cap, "/tmp");
#endif
}

/* ================================================================== */
/* 浏览文件夹 / 浏览 exe                                               */
/* ================================================================== */

#ifdef _WIN32
static void wchar_to_utf8(const wchar_t *in, char *out, size_t cap) {
    if (!in) { out[0] = 0; return; }
    WideCharToMultiByte(CP_UTF8, 0, in, -1, out, (int)cap, NULL, NULL);
}

static void browse_for_editor(char *out, size_t cap) {
    wchar_t file[1024] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = L"可执行文件 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof file / sizeof file[0];
    ofn.lpstrTitle = L"选择外部编辑器";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) wchar_to_utf8(file, out, cap);
}

static void browse_for_ssh_key(char *out, size_t cap) {
    wchar_t file[1024] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = L"私钥文件 (*.pem;*.key;*.ppk;id_*)\0*.pem;*.key;*.ppk;id_*\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof file / sizeof file[0];
    ofn.lpstrTitle = L"选择 SSH 私钥";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) wchar_to_utf8(file, out, cap);
}

static void browse_for_folder(char *out, size_t cap) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof bi);
    bi.lpszTitle = L"选择下载目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) wchar_to_utf8(path, out, cap);
        CoTaskMemFree(pidl);
    }
    if (SUCCEEDED(hr)) CoUninitialize();
}
#else
static void browse_for_editor(char *out, size_t cap) { (void)out; (void)cap; }
static void browse_for_folder(char *out, size_t cap) { (void)out; (void)cap; }
static void browse_for_folder(char *out, size_t cap) { (void)out; (void)cap; }
#endif

/* ================================================================== */
/* 文件状态                                                            */
/* ================================================================== */

static bool file_stat(const char *path, uint64_t *mtime, uint64_t *size) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return false;
    *mtime = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
    *size  = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    return true;
#else
    struct stat st;
    if (stat(path, &st) != 0) return false;
    *mtime = (uint64_t)st.st_mtime;
    *size  = (uint64_t)st.st_size;
    return true;
#endif
}

/* 本地路径所在磁盘的剩余字节数；失败返回 0 */
static uint64_t local_free_space(const char *path) {
#ifdef _WIN32
    ULARGE_INTEGER free_bytes;
    /* 目录可能不存在，取它所在的盘符：找到第一个 :\\ */
    char root[4] = { 0 };
    if (path[0] && path[1] == ':') {
        root[0] = path[0];
        root[1] = ':';
        root[2] = '\\';
        root[3] = 0;
    } else {
        /* 相对路径：用当前目录的盘符 */
        char cwd[MAX_PATH];
        if (!GetCurrentDirectoryA(MAX_PATH, cwd)) return 0;
        root[0] = cwd[0]; root[1] = ':'; root[2] = '\\'; root[3] = 0;
    }
    if (!GetDiskFreeSpaceExA(root, &free_bytes, NULL, NULL)) return 0;
    return (uint64_t)free_bytes.QuadPart;
#else
    struct statvfs sv;
    /* 路径可能不存在：逐级往上找存在的父目录 */
    char probe[1024];
    snprintf(probe, sizeof probe, "%s", path);
    for (;;) {
        if (statvfs(probe, &sv) == 0)
            return (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
        char *slash = strrchr(probe, '/');
        if (!slash) break;
        if (slash == probe) { probe[1] = 0; }   /* 到 / 为止 */
        else *slash = 0;
    }
    return 0;
#endif
}

/* 远程目录所在文件系统的剩余字节数；失败返回 0（0 表示"未知"，调用方不拦） */
static uint64_t remote_free_space(int tab_idx, const char *dir) {
    if (tab_idx < 0 || tab_idx >= g_tab_count || !g_tab_ssh[tab_idx]) return 0;
    /* -P 可移植输出，-k 以 1K 块为单位（POSIX 标准，所有 df 都支持） */
    char escaped[1024];
    size_t o = 0;
    for (const char *p = dir; *p && o + 4 < sizeof escaped; p++) {
        if (*p == '\'') { memcpy(escaped + o, "'\\''", 4); o += 4; }
        else escaped[o++] = *p;
    }
    escaped[o] = 0;

    char cmd[1280];
    snprintf(cmd, sizeof cmd, "df -P -k '%s' 2>/dev/null | tail -n 1", escaped);
    char out[512] = "";
    if (!exec_remote(g_tab_ssh[tab_idx], cmd, out, sizeof out)) return 0;

    /* 输出形如：/dev/sda1  104857600  20971520  83886080  20% / */
    /* 注意：df 的"Available"是 1K 块数 */
    char fs[128];
    unsigned long long total = 0, used = 0, avail = 0;
    int pct = 0;
    char mnt[256];
    char *line = out;
    while (*line) {
        if (sscanf(line, "%127s %llu %llu %llu %d%% %255s",
                   fs, &total, &used, &avail, &pct, mnt) == 6) {
            return (uint64_t)avail * 1024ULL;
        }
        char *nl = strchr(line, '\n');
        if (!nl) break;
        line = nl + 1;
    }
    return 0;
}

/* 上传前检查远程空间；返回 false 表示空间不足，应中止 */
static bool check_remote_space_for_upload(cui_ctx *ui, int tab_idx,
                                          const char *remote_path, uint64_t total_size) {
    /* 从 remote_path 提取目录 */
    char remote_dir[1024];
    snprintf(remote_dir, sizeof remote_dir, "%s", remote_path);
    char *slash = strrchr(remote_dir, '/');
    if (slash && slash != remote_dir) *slash = 0;
    else if (slash == remote_dir) remote_dir[1] = 0;   /* 根目录 "/" */

    uint64_t need = total_size + 1024 * 1024 * 50;   /* +1MB 余量 */
    uint64_t have = remote_free_space(tab_idx, remote_dir);
    if (have > 0 && have < need) {
        log_status(T("远程磁盘空间不足：需要 %.1f MB，剩余 %.1f MB",
                     "Not enough remote space: need %.1f MB, have %.1f MB"),
                   need / (1024.0 * 1024.0), have / (1024.0 * 1024.0));
        if (ui) {
            char title[256];
            snprintf(title, sizeof title,
                     T("远程磁盘空间不足", "Not enough remote disk space"));
            cui_toast(ui, title,
                      .description = remote_dir,
                      .tone = CUI_TONE_DANGER,
                      .duration = 3.0f);
        }
        return false;
    }
    return true;
}

static bool copy_file_remote(int tab_idx, const char *src, const char *dst) {
    if (tab_idx < 0 || tab_idx >= g_tab_count) return false;
    sftp_session sftp = g_tab_sftp[tab_idx];
    if (!sftp) return false;

    sftp_file in = sftp_open(sftp, src, O_RDONLY, 0);
    if (!in) {
        log_status(T("打开源文件失败：%s", "Cannot open source: %s"), ssh_get_error(g_tab_ssh[tab_idx]));
        return false;
    }
    sftp_file out = sftp_open(sftp, dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!out) {
        log_status(T("创建目标文件失败：%s", "Cannot create target: %s"), ssh_get_error(g_tab_ssh[tab_idx]));
        sftp_close(in);
        return false;
    }

    char buf[8192];
    long long n;
    uint64_t total = 0;
    while ((n = (long long)sftp_read(in, buf, sizeof buf)) > 0) {
        if (sftp_write(out, buf, (size_t)n) != (ssize_t)n) {
            log_status(T("写入目标文件失败：%s", "Write failed: %s"), ssh_get_error(g_tab_ssh[tab_idx]));
            sftp_close(in);
            sftp_close(out);
            return false;
        }
        total += (uint64_t)n;
    }
    sftp_close(in);
    sftp_close(out);
    log_status(T("已复制文件：%s → %s（%llu 字节）", "Copied: %s → %s (%llu bytes)"), src, dst, (unsigned long long)total);
    return true;
}

static bool copy_dir_remote(int tab_idx, const char *src, const char *dst) {
    if (tab_idx < 0 || tab_idx >= g_tab_count) return false;
    sftp_session sftp = g_tab_sftp[tab_idx];
    if (!sftp) return false;

    sftp_mkdir(sftp, dst, 0755);

    sftp_dir dir = sftp_opendir(sftp, src);
    if (!dir) {
        log_status(T("打开源目录失败：%s", "Cannot open source folder: %s"), ssh_get_error(g_tab_ssh[tab_idx]));
        return false;
    }
    sftp_attributes attr;
    while ((attr = sftp_readdir(sftp, dir)) != NULL) {
        if (!strcmp(attr->name, ".") || !strcmp(attr->name, "..")) {
            sftp_attributes_free(attr);
            continue;
        }
        char s_sub[1024], d_sub[1024];
        snprintf(s_sub, sizeof s_sub, "%s/%s", src, attr->name);
        snprintf(d_sub, sizeof d_sub, "%s/%s", dst, attr->name);
        if (attr->type == SSH_FILEXFER_TYPE_DIRECTORY) {
            copy_dir_remote(tab_idx, s_sub, d_sub);
        } else {
            copy_file_remote(tab_idx, s_sub, d_sub);
        }
        sftp_attributes_free(attr);
    }
    sftp_closedir(dir);
    return true;
}

static bool copy_via_shell(int tab_idx, const char *src, const char *dst, bool is_dir) {
    if (tab_idx < 0 || tab_idx >= g_tab_count || !g_tab_ssh[tab_idx]) return false;
    char cmd[2048];
    if (is_dir)
        snprintf(cmd, sizeof cmd, "cp -r '%s' '%s' 2>&1", src, dst);
    else
        snprintf(cmd, sizeof cmd, "cp '%s' '%s' 2>&1", src, dst);
    /* 用 exec_remote 执行 */
    char out[512] = "";
    if (!exec_remote(g_tab_ssh[tab_idx], cmd, out, sizeof out)) return false;
    /* cp 输出为空 = 成功 */
    return out[0] == 0;
}

static bool remote_path_is_dir(int tab_idx, const char *path) {
    if (tab_idx < 0 || tab_idx >= g_tab_count) return false;
    sftp_session sftp = g_tab_sftp[tab_idx];
    if (!sftp) return false;
    sftp_attributes attr = sftp_stat(sftp, path);
    if (!attr) return false;
    bool is_dir = (attr->type == SSH_FILEXFER_TYPE_DIRECTORY);
    sftp_attributes_free(attr);
    return is_dir;
}


static void cmds_file_path(char *out, size_t cap) {
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata) snprintf(out, cap, "%s\\CUI\\commands.ini", appdata);
    else snprintf(out, cap, "commands.ini");
#else
    snprintf(out, cap, "commands.ini");
#endif
}

/* 保存命令列表到 INI */
static void save_cmds(void) {
    char path[512];
    cmds_file_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < g_cmd_count; i++) {
        cmd_entry *c = &g_cmds[i];
        fprintf(f, "[cmd_%d]\n", i + 1);
        fprintf(f, "name=%s\n", c->name);
        fprintf(f, "created=%lld\n", (long long)c->created);
        fprintf(f, "add_cr=%d\n", c->add_cr ? 1 : 0);
        /* 命令可能含换行，用 \n 转义 */
        fprintf(f, "command=");
        for (const char *p = c->command; *p; p++) {
            if (*p == '\n') fprintf(f, "\\n");
            else if (*p == '\\') fprintf(f, "\\\\");
            else fputc(*p, f);
        }
        fprintf(f, "\n\n");
    }
    fclose(f);
}

/* 从 INI 加载命令列表 */
static void load_cmds(void) {
    char path[512];
    cmds_file_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[4096];
    g_cmd_count = 0;
    cmd_entry *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == ';' || *p == '#') continue;
        if (*p == '[') {
            if (g_cmd_count >= MAX_CMDS) break;
            cur = &g_cmds[g_cmd_count++];
            memset(cur, 0, sizeof *cur);
            continue;
        }
        if (!cur) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = p, *val = eq + 1;
        char *nl = strchr(val, '\n'); if (nl) *nl = 0;
        if (!strcmp(key, "name")) snprintf(cur->name, sizeof cur->name, "%s", val);
        else if (!strcmp(key, "created")) cur->created = (time_t)strtoll(val, NULL, 10);
        else if (!strcmp(key, "add_cr")) cur->add_cr = atoi(val) != 0;
        else if (!strcmp(key, "command")) {
            /* 解析 \n 和 \\ 转义 */
            int n = 0;
            for (const char *q = val; *q && n < (int)sizeof(cur->command) - 1; q++) {
                if (*q == '\\' && q[1]) {
                    if (q[1] == 'n') { cur->command[n++] = '\n'; q++; }
                    else if (q[1] == '\\') { cur->command[n++] = '\\'; q++; }
                    else cur->command[n++] = *q;
                } else cur->command[n++] = *q;
            }
            cur->command[n] = 0;
        }
    }
    fclose(f);
}

static void commands_panel(cui_ctx *ui) {
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 4) {
        /* 标题行 + 加号 + 执行选中 */
        cui_row(ui, .width = CUI_FILL, .pad_x = 4, .pad_y = 4, .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .width = CUI_GROW(1)) {
                cui_text(ui, T("命令列表", "Commands"), .font = CUI_FONT_MEDIUM, .size = 13);
            }
            /* 执行选中的命令：图标按钮，未选中时置灰 */
            if (cui_icon_button(ui, CUI_ICON_PLAY,
                                .tooltip = T("执行选中的命令", "Run selected command"),
                                //.variant = CUI_BUTTON_PRIMARY,
                                .disabled = (g_cmd_selected < 0 || g_cmd_selected >= g_cmd_count))) {
                if (g_cmd_selected >= 0 && g_cmd_selected < g_cmd_count) {
                    cmd_entry *c = &g_cmds[g_cmd_selected];
                    if (g_ssh_tab >= 0 && g_tab_chan[g_ssh_tab]) {
                        ssh_channel_write(g_tab_chan[g_ssh_tab], c->command,
                                          (uint32_t)strlen(c->command));
                        if (c->add_cr)
                            ssh_channel_write(g_tab_chan[g_ssh_tab], "\r", 1);
                        log_status(T("执行命令：%s", "Run Command: %s"), c->name);
                    } else {
                        log_status(T("没有打开的终端，无法执行", "No open terminal to run in"));
                    }
                }
            }
            /* 新建命令 */
            if (cui_icon_button(ui, CUI_ICON_PLUS, .tooltip = T("添加命令", "Add command"))) {
                g_cmd_add_dlg = true;
                g_cmd_add_name[0] = 0;
                g_cmd_add_body[0] = 0;
                g_cmd_add_cr = true;
                g_cmd_edit_idx = -1;
            }
        }
        hline(ui);

        /* 表头 */
        cui_row(ui, .width = CUI_FILL, .gap = 4, .pad_y = 4, .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .width = CUI_GROW(1)) {
                cui_text(ui, T("名称", "Name"), .size = 11,
                         .color = CUI_COLOR_TEXT_SECONDARY, .align = CUI_TEXT_LEFT);
            }
            cui_box(ui, .width = CUI_PX(140)) {
                cui_text(ui, T("创建日期", "Created"), .size = 11,
                         .color = CUI_COLOR_TEXT_SECONDARY);
            }
            cui_box(ui, .width = CUI_PX(140)) {
                cui_text(ui, T("操作", "Actions"), .size = 11,
                         .color = CUI_COLOR_TEXT_SECONDARY);
            }
        }
        hline(ui);

        /* 命令列表 */
        int del_idx = -1;
        int edit_idx = -1;
        cui_scroll(ui, .id = "cmd-list-scroll", .height = CUI_GROW(1)) {
            for (int i = 0; i < g_cmd_count; i++) {
                cui_push_id_int(ui, i);
                cmd_entry *c = &g_cmds[i];
                cui_row(ui, .width = CUI_FILL, .gap = 4, .pad_y = 2, .align = CUI_ALIGN_CENTER) {
                    /* ---- 名称：左对齐 + 单击选中 + 双击编辑 ---- */
                    char name_id_str[64];
                    snprintf(name_id_str, sizeof name_id_str, "cmd-name-%d", i);
                    cui_id name_id = cui_make_id(ui, name_id_str);
                    int name_node = cui__node_new(ui, name_id);
                    ui->nodes[name_node].w = CUI_GROW(1);
                    ui->nodes[name_node].h = CUI_PX(24);
                    ui->nodes[name_node].radius = CUI_CORNERS(4);
                    ui->nodes[name_node].pad[0] = 6;   /* pad_left */
                    if (g_cmd_selected == i)
                        ui->nodes[name_node].bg = cui_alpha(CUI_C(ui, ACCENT), 0.18f);
                    cui__open(ui, name_node);
                        cui_row(ui, .width = CUI_FILL, .height = CUI_FILL,
                                .align = CUI_ALIGN_CENTER) {
                            cui_text(ui, c->name, .size = 12, .nowrap = true,
                                     .align = CUI_TEXT_LEFT, .width = CUI_FILL);
                        }
                    cui__close(ui);
                    cui__mark_interactive(ui, name_node, 0);

                    cui_interact nvr = cui__interact(ui, name_id, 0);
                    if (nvr.hovered) cui__set_cursor(ui, CUI_CURSOR_HAND);
                    static bool prev_pressed_name[256];
                    bool name_clicked = nvr.pressed && !prev_pressed_name[i];
                    prev_pressed_name[i] = nvr.pressed;
                    if (name_clicked) {
                        g_cmd_selected = i;
                        /* 双击检测 */
                        static double last_time[256];
                        static int last_idx[256];
                        double now = cui_time(ui);
                        if (last_idx[i] == i && now - last_time[i] < 0.35) {
                            last_idx[i] = -1;
                            edit_idx = i;
                        } else {
                            last_idx[i] = i;
                            last_time[i] = now;
                        }
                    }

                    /* ---- 创建日期 ---- */
                    cui_box(ui, .width = CUI_PX(140)) {
                        char buf[32];
                        struct tm *tmv = localtime(&c->created);
                        if (tmv) strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", tmv);
                        else buf[0] = 0;
                        cui_text(ui, buf, .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
                    }

                    /* ---- 操作：编辑 / 删除 / 执行 / 复制 ---- */
                    cui_box(ui, .width = CUI_PX(140)) {
                        cui_row(ui, .gap = 2, .align = CUI_ALIGN_CENTER) {
                            if (cui_icon_button(ui, CUI_ICON_PENCIL,
                                                .tooltip = T("编辑", "Edit"),
                                                .size = CUI_SIZE_SM)) {
                                edit_idx = i;
                            }
                            if (cui_icon_button(ui, CUI_ICON_TRASH_2,
                                                .tooltip = T("删除", "Delete"),
                                                .size = CUI_SIZE_SM)) {
                                del_idx = i;
                            }
                            if (cui_icon_button(ui, CUI_ICON_PLAY,
                                                .tooltip = T("执行", "Run"),
                                                .size = CUI_SIZE_SM)) {
                                if (g_ssh_tab >= 0 && g_tab_chan[g_ssh_tab]) {
                                    ssh_channel_write(g_tab_chan[g_ssh_tab], c->command,
                                                      (uint32_t)strlen(c->command));
                                    if (c->add_cr)
                                        ssh_channel_write(g_tab_chan[g_ssh_tab], "\r", 1);
                                    log_status(T("执行命令：%s", "Run command: %s"), c->name);
                                } else {
                                    log_status_toast(ui, CUI_TONE_WARNING, T("没有打开的终端，无法执行", "No open terminal to run in"));
                                }
                            }
                            if (cui_icon_button(ui, CUI_ICON_COPY,
                                                .tooltip = T("复制", "Copy"),
                                                .size = CUI_SIZE_SM)) {
                                if (ui->platform.set_clipboard) {
                                    char full[4096];
                                    snprintf(full, sizeof full, "%s%s",
                                             c->command, c->add_cr ? "\r" : "");
                                    ui->platform.set_clipboard(ui->platform.user, full);
                                    log_status_toast(ui, CUI_TONE_SUCCESS, T("已复制：%s", "Copied: %s"), c->name);
                                }
                            }
                        }
                    }
                }
                cui_pop_id(ui);
            }
            if (g_cmd_count == 0) {
                cui_text(ui, T("暂无命令，点右上角 + 添加", "No commands — use + above to add"),
                         .color = CUI_COLOR_TEXT_SECONDARY);
            }
        }

        /* 延迟删除 */
        if (del_idx >= 0) {
            for (int j = del_idx; j < g_cmd_count - 1; j++)
                memcpy(&g_cmds[j], &g_cmds[j + 1], sizeof(cmd_entry));
            g_cmd_count--;
            /* 修正选中索引 */
            if (g_cmd_selected == del_idx) g_cmd_selected = -1;
            else if (g_cmd_selected > del_idx) g_cmd_selected--;
            save_cmds();
        }

        /* 延迟编辑 */
        if (edit_idx >= 0) {
            cmd_entry *c = &g_cmds[edit_idx];
            snprintf(g_cmd_add_name, sizeof g_cmd_add_name, "%s", c->name);
            snprintf(g_cmd_add_body, sizeof g_cmd_add_body, "%s", c->command);
            g_cmd_add_cr = c->add_cr;
            g_cmd_edit_idx = edit_idx;
            g_cmd_add_dlg = true;
        }
    }
}

static void linux_cmds_init(void) {
    if (g_linux_cmds) return;
    g_linux_cmd_cap = LINUX_BUILTIN_COUNT + 16;
    g_linux_cmds = (linux_cmd *)malloc(sizeof(linux_cmd) * g_linux_cmd_cap);
    for (int i = 0; i < LINUX_BUILTIN_COUNT; i++)
        g_linux_cmds[i] = g_linux_builtin[i];
    g_linux_cmd_count = LINUX_BUILTIN_COUNT;
}

static void linux_cmds_free(void) {
    free(g_linux_cmds);
    g_linux_cmds = NULL;
    g_linux_cmd_count = g_linux_cmd_cap = 0;
}

static int linux_cmds_add(const linux_cmd *c) {
    linux_cmds_init();
    if (g_linux_cmd_count >= MAX_LINUX_CMDS) return -1;
    if (g_linux_cmd_count >= g_linux_cmd_cap) {
        g_linux_cmd_cap = g_linux_cmd_cap ? g_linux_cmd_cap * 2 : 64;
        g_linux_cmds = (linux_cmd *)realloc(g_linux_cmds, sizeof(linux_cmd) * g_linux_cmd_cap);
    }
    g_linux_cmds[g_linux_cmd_count] = *c;
    return g_linux_cmd_count++;
}

static void linux_cmds_remove(int idx) {
    if (idx < 0 || idx >= g_linux_cmd_count) return;
    if (g_linux_cmds[idx].builtin) return;
    for (int i = idx; i < g_linux_cmd_count - 1; i++)
        g_linux_cmds[i] = g_linux_cmds[i + 1];
    g_linux_cmd_count--;
}

static void linux_cmds_ini_path(char *out, size_t cap) {
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata) snprintf(out, cap, "%s\\CUI\\linux_commands.ini", appdata);
    else snprintf(out, cap, "linux_commands.ini");
#else
    snprintf(out, cap, "linux_commands.ini");
#endif
}

static void save_linux_cmds(void) {
    char path[512]; linux_cmds_ini_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    int n = 0;
    for (int i = 0; i < g_linux_cmd_count; i++) {
        if (g_linux_cmds[i].builtin) continue;
        n++;
        linux_cmd *c = &g_linux_cmds[i];
        fprintf(f, "[cmd_%d]\n", n);
        fprintf(f, "category_zh=%s\n", c->category_zh);
        fprintf(f, "category_en=%s\n", c->category_en);
        fprintf(f, "name_zh=%s\n", c->name_zh);
        fprintf(f, "name_en=%s\n", c->name_en);
        fprintf(f, "desc_zh=%s\n", c->desc_zh);
        fprintf(f, "desc_en=%s\n", c->desc_en);
        fprintf(f, "body=");
        for (const char *p = c->body; *p; p++) {
            if (*p == '\n') fprintf(f, "\\n");
            else if (*p == '\\') fprintf(f, "\\\\");
            else fputc(*p, f);
        }
        fprintf(f, "\n");
        for (int k = 0; k < 5; k++) {
            fprintf(f, "hint%d_zh=%s\n", k + 1, c->hint_zh[k]);
            fprintf(f, "hint%d_en=%s\n", k + 1, c->hint_en[k]);
        }
        fprintf(f, "example=%s\n", c->example);
        fprintf(f, "add_cr=%d\n", c->add_cr ? 1 : 0);
        fprintf(f, "\n");
    }
    fclose(f);
}

static void load_linux_cmds(void) {
    linux_cmds_init();
    char path[512]; linux_cmds_ini_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[2048];
    linux_cmd cur; memset(&cur, 0, sizeof cur); cur.builtin = false;
    int in_cmd = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == ';' || *p == '#') continue;
        if (*p == '[') {
            if (in_cmd) linux_cmds_add(&cur);
            memset(&cur, 0, sizeof cur); cur.builtin = false;
            in_cmd = 1;
            continue;
        }
        if (!in_cmd) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = p, *val = eq + 1;
        char *nl = strchr(val, '\n'); if (nl) *nl = 0;
        if (!strcmp(key, "category_zh")) snprintf(cur.category_zh, sizeof cur.category_zh, "%s", val);
        else if (!strcmp(key, "category_en")) snprintf(cur.category_en, sizeof cur.category_en, "%s", val);
        else if (!strcmp(key, "name_zh")) snprintf(cur.name_zh, sizeof cur.name_zh, "%s", val);
        else if (!strcmp(key, "name_en")) snprintf(cur.name_en, sizeof cur.name_en, "%s", val);
        else if (!strcmp(key, "desc_zh")) snprintf(cur.desc_zh, sizeof cur.desc_zh, "%s", val);
        else if (!strcmp(key, "desc_en")) snprintf(cur.desc_en, sizeof cur.desc_en, "%s", val);
        else if (!strcmp(key, "body")) {
            int n = 0;
            for (const char *q = val; *q && n < (int)sizeof(cur.body) - 1; q++) {
                if (*q == '\\' && q[1]) {
                    if (q[1] == 'n') { cur.body[n++] = '\n'; q++; }
                    else if (q[1] == '\\') { cur.body[n++] = '\\'; q++; }
                    else cur.body[n++] = *q;
                } else cur.body[n++] = *q;
            }
            cur.body[n] = 0;
        }
        else if (!strncmp(key, "hint", 4)) {
            /* hintN_zh / hintN_en */
            int n = 0, off = 0;
            if (sscanf(key + 4, "%d_%n", &n, &off) == 1 && n >= 1 && n <= 5) {
                const char *field = key + 4 + off;
                if (!strcmp(field, "zh")) snprintf(cur.hint_zh[n-1], sizeof cur.hint_zh[n-1], "%s", val);
                else if (!strcmp(field, "en")) snprintf(cur.hint_en[n-1], sizeof cur.hint_en[n-1], "%s", val);
            }
        }
        else if (!strcmp(key, "example")) snprintf(cur.example, sizeof cur.example, "%s", val);
        else if (!strcmp(key, "add_cr")) cur.add_cr = atoi(val) != 0;
    }
    if (in_cmd) linux_cmds_add(&cur);
    fclose(f);
}

static int linux_cmd_cmp(const void *a, const void *b) {
    const linux_cmd *ca = (const linux_cmd *)a;
    const linux_cmd *cb = (const linux_cmd *)b;
    int col = g_linux_sort.column;
    int r = 0;
    if (col < 0) {
        /* 默认：分类 → 名称 */
        r = strcmp(LCMD_CAT(ca), LCMD_CAT(cb));
        if (r == 0) r = strcmp(LCMD_NAME(ca), LCMD_NAME(cb));
    } else if (col == 0) {
        r = strcmp(LCMD_CAT(ca), LCMD_CAT(cb));
        if (r == 0) r = strcmp(LCMD_NAME(ca), LCMD_NAME(cb));
    } else if (col == 1) {
        r = strcmp(LCMD_NAME(ca), LCMD_NAME(cb));
    } else if (col == 2) {
        r = strcmp(ca->body, cb->body);
    }
    return g_linux_sort.descending ? -r : r;
}

/* 添加/编辑对话框的缓冲 */
static linux_cmd g_linux_edit_buf;

static void linux_cmds_panel(cui_ctx *ui) {
    linux_cmds_init();

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 4) {
        /* 标题行：标题 + 搜索 + 添加 */
        /* 分类列表：从 g_linux_cmds 动态收集，去重，按当前语言显示 */
        static char cat_names[64][64];
        static const char *cat_ptrs[64];
        int cat_n = 0;
        snprintf(cat_names[cat_n], sizeof cat_names[cat_n], "%s", T("全部", "All"));
        cat_ptrs[cat_n] = cat_names[cat_n];
        cat_n++;
        for (int i = 0; i < g_linux_cmd_count && cat_n < 64; i++) {
            const char *cat = LCMD_CAT(&g_linux_cmds[i]);
            bool found = false;
            for (int j = 1; j < cat_n; j++)
                if (!strcmp(cat_ptrs[j], cat)) { found = true; break; }
            if (!found) {
                snprintf(cat_names[cat_n], sizeof cat_names[cat_n], "%s", cat);
                cat_ptrs[cat_n] = cat_names[cat_n];
                cat_n++;
            }
        }
        if (g_linux_cat_filter >= cat_n) g_linux_cat_filter = 0;

        cui_row(ui, .width = CUI_FILL, .pad_x = 4, .pad_y = 4, .align = CUI_ALIGN_CENTER, .gap = 6) {
            cui_box(ui, .width = CUI_GROW(1)) {
                cui_text(ui, T("常用指令", "Common commands"),
                         .font = CUI_FONT_MEDIUM, .size = 13);
            }
            cui_box(ui, .width = CUI_PX(200)) {
                cui_search(ui, "linux-filter", g_linux_filter, sizeof g_linux_filter,
                           .placeholder = T("过滤名称/代码/描述...", "Filter name/code/desc..."));
            }
            cui_box(ui, .width = CUI_PX(140)) {
                cui_select(ui, "linux-cat", cat_ptrs, cat_n, &g_linux_cat_filter,
                           .placeholder = T("分类", "Category"));
            }
            if (cui_icon_button(ui, CUI_ICON_REFRESH_CW,
                                .tooltip = T("刷新列表", "Refresh list"))) {
                /* 重新从外部文件加载自定义命令，保留内置 */
                linux_cmds_free();
                load_linux_cmds();
                /* 恢复初始状态：默认排序 + 清空过滤 + 分类选“全部” */
                g_linux_sort.column = -1;
                g_linux_sort.descending = false;
                g_linux_filter[0] = 0;
                g_linux_cat_filter = 0;
            }
            if (cui_icon_button(ui, CUI_ICON_PLUS, .tooltip = T("添加自定义命令", "Add custom command"))) {
                memset(&g_linux_edit_buf, 0, sizeof g_linux_edit_buf);
                g_linux_edit_buf.builtin = false;
                g_linux_edit_buf.add_cr = true;
                snprintf(g_linux_edit_buf.category_zh, sizeof g_linux_edit_buf.category_zh, "自定义");
                snprintf(g_linux_edit_buf.category_en, sizeof g_linux_edit_buf.category_en, "Custom");
                g_linux_edit_idx = -1;
                g_linux_add_dlg = true;
            }
        }
        
        hline(ui);

        /* 表头：分类 / 名称 / 代码 / 操作 */
        cui_row(ui, .width = CUI_FILL, .gap = 4, .pad_y = 4, .align = CUI_ALIGN_CENTER) {
            sortable_header(ui, "linux", T("分类", "Category"), 90, false, CUI_TEXT_LEFT, &g_linux_sort, 0);
            sortable_header(ui, "linux", T("名称 / 描述", "Name / Description"), 0, true, CUI_TEXT_LEFT, &g_linux_sort, 1);
            sortable_header(ui, "linux", T("代码", "Code"), 280, false, CUI_TEXT_LEFT, &g_linux_sort, 2);
            cui_box(ui, .width = CUI_PX(84)) {
                cui_text(ui, T("操作", "Actions"), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
            }
        }
        hline(ui);

        /* 过滤 + 排序 */
        int visible[256]; int vn = 0;
        const char *sel_cat = (g_linux_cat_filter > 0 && g_linux_cat_filter < cat_n)
                              ? cat_ptrs[g_linux_cat_filter] : NULL;
        for (int i = 0; i < g_linux_cmd_count; i++) {
            linux_cmd *c = &g_linux_cmds[i];
            if (sel_cat && strcmp(LCMD_CAT(c), sel_cat) != 0) continue;
            if (g_linux_filter[0]) {
                if (!str_contains_ci(LCMD_NAME(c), g_linux_filter) &&
                    !str_contains_ci(LCMD_DESC(c), g_linux_filter) &&
                    !str_contains_ci(c->body, g_linux_filter) &&
                    !str_contains_ci(LCMD_CAT(c), g_linux_filter))
                    continue;
            }
            visible[vn++] = i;
        }
        /* 排序：用指针数组，不移动原数组 */
        linux_cmd *ptrs[256];
        for (int i = 0; i < vn; i++) ptrs[i] = &g_linux_cmds[visible[i]];
        /* 手动插入排序，保持稳定 */
        for (int i = 1; i < vn; i++) {
            linux_cmd *key = ptrs[i];
            int j = i - 1;
            while (j >= 0 && linux_cmd_cmp(ptrs[j], key) > 0) {
                ptrs[j + 1] = ptrs[j];
                j--;
            }
            ptrs[j + 1] = key;
        }

        int run_idx = -1;
        int edit_idx = -1;
        int del_idx = -1;
        cui_scroll(ui, .id = "linux-cmds-scroll", .height = CUI_GROW(1)) {
            for (int k = 0; k < vn; k++) {
                linux_cmd *c = ptrs[k];
                int orig = (int)(c - g_linux_cmds);

                cui_push_id_int(ui, orig);
                char id[64]; snprintf(id, sizeof id, "lx-row-%d", orig);
                cui_id row_id = cui_make_id(ui, id);
                int row = cui__node_new(ui, row_id);
                ui->nodes[row].w = CUI_FILL;
                ui->nodes[row].h = CUI_PX(40);
                cui__open(ui, row);
                    cui_row(ui, .width = CUI_FILL, .gap = 4, .pad_x = 4, .align = CUI_ALIGN_CENTER) {
                        cui_box(ui, .width = CUI_PX(90)) {
                            cui_text(ui, LCMD_CAT(c), .size = 11,
                                     .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
                        }
                        cui_box(ui, .width = CUI_GROW(1)) {
                            cui_column(ui, .gap = 2) {
                                cui_text(ui, LCMD_NAME(c), .size = 12, .nowrap = true);
                                cui_text(ui, LCMD_DESC(c), .size = 10,
                                         .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
                            }
                        }
                        cui_box(ui, .width = CUI_PX(280)) {
                            cui_text(ui, c->body, .font = CUI_FONT_MONO, .size = 11,
                                     .nowrap = true, .color = CUI_COLOR_TEXT_SECONDARY);
                        }
                        cui_box(ui, .width = CUI_PX(84)) {
                            cui_row(ui, .gap = 2, .align = CUI_ALIGN_CENTER) {
                                if (cui_icon_button(ui, CUI_ICON_PLAY,
                                                    .tooltip = T("执行", "Run"), .size = CUI_SIZE_SM))
                                    run_idx = orig;
                                if (!c->builtin) {
                                    if (cui_icon_button(ui, CUI_ICON_PENCIL,
                                                        .tooltip = T("编辑", "Edit"), .size = CUI_SIZE_SM))
                                        edit_idx = orig;
                                    if (cui_icon_button(ui, CUI_ICON_TRASH_2,
                                                        .tooltip = T("删除", "Delete"), .size = CUI_SIZE_SM))
                                        del_idx = orig;
                                }
                            }
                        }
                    }
                cui__close(ui);
                cui__mark_interactive(ui, row, 0);

                cui_interact vr = cui__interact(ui, row_id, 0);
                if (vr.hovered) cui__set_cursor(ui, CUI_CURSOR_HAND);
                static bool prev_pressed[256];
                bool clicked_now = vr.pressed && !prev_pressed[orig];
                prev_pressed[orig] = vr.pressed;
                if (clicked_now) {
                    static double last_time[256];
                    static int last_idx[256];
                    double now = cui_time(ui);
                    if (last_idx[orig] == orig && now - last_time[orig] < 0.35) {
                        last_idx[orig] = -1;
                        run_idx = orig;
                    } else {
                        last_idx[orig] = orig;
                        last_time[orig] = now;
                    }
                }
                cui_pop_id(ui);
            }
            if (vn == 0) {
                cui_text(ui, T("没有匹配的命令", "No matching commands"),
                         .color = CUI_COLOR_TEXT_SECONDARY);
            }
        }

        if (run_idx >= 0) {
            g_linux_run_idx = run_idx;
            g_linux_run_dlg = true;
            for (int k = 0; k < 5; k++) g_linux_args[k][0] = 0;
            snprintf(g_linux_preview, sizeof g_linux_preview, "%s", g_linux_cmds[run_idx].body);
        }
        if (edit_idx >= 0) {
            g_linux_edit_buf = g_linux_cmds[edit_idx];
            g_linux_edit_idx = edit_idx;
            g_linux_edit_dlg = true;
        }
        if (del_idx >= 0) {
            g_linux_del_idx = del_idx;
            g_linux_del_dlg = true;
        }
    }
}

static void linux_run_dialog(cui_ctx *ui) {
    if (!g_linux_run_dlg) return;
    if (g_linux_run_idx < 0 || g_linux_run_idx >= g_linux_cmd_count) {
        g_linux_run_dlg = false;
        return;
    }
    linux_cmd *c = &g_linux_cmds[g_linux_run_idx];

    cui_dialog(ui, T("执行命令", "Run command"), &g_linux_run_dlg, .width = 640) {
        cui_text(ui, LCMD_NAME(c), .font = CUI_FONT_MEDIUM);
        cui_text(ui, LCMD_DESC(c), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
        cui_gap(ui, 6);

        /* 参数输入 */
        for (int k = 0; k < 5; k++) {
            const char *hint = LCMD_HINT(c, k);
            if (!hint[0]) continue;
            char label[64];
            snprintf(label, sizeof label, T("参数%d", "Param %d"), k + 1);
            char id[16]; snprintf(id, sizeof id, "lxarg%d", k);
            cui_input(ui, id, g_linux_args[k], sizeof g_linux_args[k],
                      .label = label, .placeholder = hint);
        }

        cui_gap(ui, 6);
        cui_text(ui, T("命令（可编辑）", "Command (editable)"),
                 .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        cui_code_editor(ui, "lx-preview", g_linux_preview, sizeof g_linux_preview,
                        .lang = "shell", .height = 80, .no_line_numbers = true);

        if (c->example[0]) {
            cui_text(ui, T("示例：", "Example:"), .size = 11, .color = CUI_COLOR_TEXT_MUTED);
            cui_text(ui, c->example, .size = 11, .font = CUI_FONT_MONO,
                     .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
        }

        cui_dialog_footer(ui) {
            if (cui_button(ui, T("取消", "Cancel"), .variant = CUI_BUTTON_GHOST))
                g_linux_run_dlg = false;

            if (cui_button(ui, T("填入", "Fill"), .variant = CUI_BUTTON_OUTLINE)) {
                snprintf(g_linux_preview, sizeof g_linux_preview, "%s", c->body);
                for (int k = 4; k >= 0; k--) {
                    const char *hint = LCMD_HINT(c, k);
                    if (!hint[0] || !g_linux_args[k][0]) continue;
                    char pat[8];
                    snprintf(pat, sizeof pat, "{%d}", k + 1);
                    char out[2048]; int o = 0;
                    const char *p = g_linux_preview;
                    size_t plen = strlen(pat);
                    while (*p && o < (int)sizeof out - 1) {
                        if (!strncmp(p, pat, plen)) {
                            size_t alen = strlen(g_linux_args[k]);
                            if (o + (int)alen >= (int)sizeof out - 1) break;
                            memcpy(out + o, g_linux_args[k], alen);
                            o += (int)alen;
                            p += plen;
                        } else out[o++] = *p++;
                    }
                    out[o] = 0;
                    snprintf(g_linux_preview, sizeof g_linux_preview, "%s", out);
                }
            }

            if (cui_button(ui, T("执行", "Run"), .variant = CUI_BUTTON_PRIMARY)) {
                if (g_ssh_tab >= 0 && g_tab_chan[g_ssh_tab] && g_linux_preview[0]) {
                    ssh_channel_write(g_tab_chan[g_ssh_tab], g_linux_preview,
                                      (uint32_t)strlen(g_linux_preview));
                    if (c->add_cr)
                        ssh_channel_write(g_tab_chan[g_ssh_tab], "\r", 1);
                    log_status_toast(ui, CUI_TONE_INFO,
                                     T("执行：%s", "Run: %s"), LCMD_NAME(c));
                } else {
                    log_status_toast(ui, CUI_TONE_WARNING,
                                     T("没有打开的终端，无法执行", "No open terminal to run in"));
                }
                g_linux_run_dlg = false;
            }
        }
    }
}

static void linux_edit_dialog(cui_ctx *ui) {
    if (!g_linux_add_dlg && !g_linux_edit_dlg) return;
    bool is_edit = g_linux_edit_dlg;
    linux_cmd *b = &g_linux_edit_buf;

    cui_dialog(ui, is_edit ? T("编辑自定义命令", "Edit custom command")
                           : T("添加自定义命令", "Add custom command"),
               is_edit ? &g_linux_edit_dlg : &g_linux_add_dlg, .width = 640) {

        /* ← 新增：表单主体放进滚动区 */
        cui_scroll(ui, .id = "lxe-scroll", .height = CUI_PX(460)) {
            cui_column(ui, .width = CUI_FILL, .gap = 6) {
                /* 分类 */
                cui_text(ui, T("分类", "Category"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_input(ui, "lxe-cat-zh", b->category_zh, sizeof b->category_zh,
                          .label = T("中文分类", "Chinese category"));
                cui_input(ui, "lxe-cat-en", b->category_en, sizeof b->category_en,
                          .label = T("英文分类", "English category"));

                cui_gap(ui, 6);
                /* 名称与描述 */
                cui_text(ui, T("名称与描述", "Name and description"),
                         .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_input(ui, "lxe-name-zh", b->name_zh, sizeof b->name_zh, .label = T("中文名称", "Chinese name"));
                cui_input(ui, "lxe-name-en", b->name_en, sizeof b->name_en, .label = T("英文名称", "English name"));
                cui_input(ui, "lxe-desc-zh", b->desc_zh, sizeof b->desc_zh, .label = T("中文描述", "Chinese description"));
                cui_input(ui, "lxe-desc-en", b->desc_en, sizeof b->desc_en, .label = T("英文描述", "English description"));

                cui_gap(ui, 6);
                /* 命令代码 */
                cui_text(ui, T("命令代码（用 {1}..{5} 表示参数）", "Command ({1}..{5} for parameters)"),
                         .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_code_editor(ui, "lxe-body", b->body, sizeof b->body,
                                .lang = "shell", .height = 80, .no_line_numbers = true);

                cui_gap(ui, 6);
                /* 参数提示 */
                cui_text(ui, T("参数提示（留空表示无此参数）", "Parameter hints (empty = no such parameter)"),
                         .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                for (int k = 0; k < 5; k++) {
                    char label_zh[32], label_en[32], id_zh[16], id_en[16];
                    snprintf(label_zh, sizeof label_zh, "参数%d 中文提示", k + 1);
                    snprintf(label_en, sizeof label_en, "Param %d Chinese hint", k + 1);
                    snprintf(id_zh, sizeof id_zh, "lxe-hz%d", k);
                    snprintf(id_en, sizeof id_en, "lxe-he%d", k);
                    cui_input(ui, id_zh, b->hint_zh[k], sizeof b->hint_zh[k], .label = label_zh);
                    cui_input(ui, id_en, b->hint_en[k], sizeof b->hint_en[k], .label = label_en);
                }

                cui_gap(ui, 6);
                /* 示例 */
                cui_text(ui, T("示例", "Example"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_input(ui, "lxe-ex", b->example, sizeof b->example);

                cui_checkbox(ui, T("执行后加回车", "Append CR after run"), &b->add_cr);
            }
        }

        cui_dialog_footer(ui) {
            if (cui_button(ui, T("取消", "Cancel"), .variant = CUI_BUTTON_GHOST)) {
                if (is_edit) g_linux_edit_dlg = false;
                else g_linux_add_dlg = false;
            }
            if (cui_button(ui, T("保存", "Save"), .variant = CUI_BUTTON_PRIMARY)) {
                if (b->name_zh[0] && b->body[0]) {
                    if (is_edit && g_linux_edit_idx >= 0 && g_linux_edit_idx < g_linux_cmd_count) {
                        g_linux_cmds[g_linux_edit_idx] = *b;
                        g_linux_cmds[g_linux_edit_idx].builtin = false;
                        log_status_toast(ui, CUI_TONE_SUCCESS,
                                         T("已保存命令：%s", "Command saved: %s"),
                                         b->name_zh);
                    } else {
                        int idx = linux_cmds_add(b);
                        if (idx >= 0) {
                            log_status_toast(ui, CUI_TONE_SUCCESS,
                                             T("已添加命令：%s", "Command added: %s"),
                                             b->name_zh);
                        } else {
                            log_status_toast(ui, CUI_TONE_DANGER,
                                             T("添加失败：命令数量已达上限", "Add failed: command limit reached"));
                        }
                    }
                    save_linux_cmds();
                    /* 清空过滤和分类，确保新命令可见 */
                    g_linux_filter[0] = 0;
                    g_linux_cat_filter = 0;
                } else {
                    log_status_toast(ui, CUI_TONE_WARNING,
                                     T("名称和命令代码不能为空", "Name and command cannot be empty"));
                    return;   /* 不关对话框，让用户补 */
                }
                if (is_edit) g_linux_edit_dlg = false;
                else g_linux_add_dlg = false;
            }
        }
    }
}

static void linux_del_dialog(cui_ctx *ui) {
    if (!g_linux_del_dlg) return;
    if (g_linux_del_idx < 0 || g_linux_del_idx >= g_linux_cmd_count ||
        g_linux_cmds[g_linux_del_idx].builtin) {
        g_linux_del_dlg = false;
        return;
    }
    linux_cmd *c = &g_linux_cmds[g_linux_del_idx];

    cui_dialog(ui, T("确认删除", "Confirm delete"), &g_linux_del_dlg, .width = 380) {
        cui_textf(ui, T("确定删除自定义命令“%s”吗？", "Delete custom command \"%s\"?"),
                  LCMD_NAME(c));
        cui_dialog_footer(ui) {
            if (cui_button(ui, T("取消", "Cancel"), .variant = CUI_BUTTON_GHOST))
                g_linux_del_dlg = false;
            if (cui_button(ui, T("删除", "Delete"), .variant = CUI_BUTTON_DANGER)) {
                linux_cmds_remove(g_linux_del_idx);
                save_linux_cmds();
                g_linux_del_dlg = false;
            }
        }
    }
}

/* ========== 传输列表持久化 ========== */
static void transfers_ini_path(char *out, size_t cap) {
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata) snprintf(out, cap, "%s\\CUI\\updownload.ini", appdata);
    else snprintf(out, cap, "updownload.ini");
#else
    snprintf(out, cap, "updownload.ini");
#endif
}

static void save_transfers(void) {
    char path[512];
    transfers_ini_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < g_transfer_count; i++) {
        transfer_entry *t = &g_transfers[i];
        fprintf(f, "[transfer_%d]\n", i + 1);
        fprintf(f, "type=%s\n",
                t->type == XFER_DOWNLOAD ? "download" :
                (t->type == XFER_UPLOAD ? "upload" : "upload_dir"));
        fprintf(f, "status=%s\n",
                t->status == XFER_PENDING ? "pending" :
                t->status == XFER_ACTIVE ? "active" :
                t->status == XFER_DONE ? "done" : "failed");
        fprintf(f, "local=%s\n", t->local);
        fprintf(f, "remote=%s\n", t->remote);
        fprintf(f, "total=%llu\n", (unsigned long long)t->total);
        fprintf(f, "done=%llu\n", (unsigned long long)t->done);
        fprintf(f, "start_time=%lld\n", (long long)t->start_time);
        fprintf(f, "err=%s\n", t->err);
        fprintf(f, "\n");
    }
    fclose(f);
}

static void load_transfers(void) {
    char path[512];
    transfers_ini_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[2048];
    transfer_entry *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == ';' || *p == '#') continue;
        if (*p == '[') {
            if (g_transfer_count >= MAX_TRANSFERS) break;
            cur = &g_transfers[g_transfer_count++];
            memset(cur, 0, sizeof *cur);
            continue;
        }
        if (!cur) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = p, *val = eq + 1;
        char *nl = strchr(val, '\n'); if (nl) *nl = 0;
        if (!strcmp(key, "type")) {
            if (!strcmp(val, "download")) cur->type = XFER_DOWNLOAD;
            else if (!strcmp(val, "upload")) cur->type = XFER_UPLOAD;
            else cur->type = XFER_UPLOAD_DIR;
        } else if (!strcmp(key, "status")) {
            if (!strcmp(val, "pending")) cur->status = XFER_PENDING;
            else if (!strcmp(val, "active")) cur->status = XFER_ACTIVE;
            else if (!strcmp(val, "done")) cur->status = XFER_DONE;
            else cur->status = XFER_FAILED;
        } else if (!strcmp(key, "local")) snprintf(cur->local, sizeof cur->local, "%s", val);
        else if (!strcmp(key, "remote")) snprintf(cur->remote, sizeof cur->remote, "%s", val);
        else if (!strcmp(key, "total")) cur->total = (uint64_t)strtoull(val, NULL, 10);
        else if (!strcmp(key, "done")) cur->done = (uint64_t)strtoull(val, NULL, 10);
        else if (!strcmp(key, "start_time")) cur->start_time = (time_t)strtoll(val, NULL, 10);
        else if (!strcmp(key, "err")) snprintf(cur->err, sizeof cur->err, "%s", val);
    }
    fclose(f);
}

/* 在逗号分隔列表里查找项（不区分大小写），返回是否找到 */
static bool list_contains(const char *list, const char *item, bool case_sensitive) {
    if (!list || !item || !*item) return false;
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        int len = (int)(end - start);
        if (len == (int)strlen(item)) {
            if (case_sensitive) {
                if (strncmp(start, item, len) == 0) return true;
            } else {
                bool match = true;
                for (int i = 0; i < len; i++) {
                    if (tolower((unsigned char)start[i]) != tolower((unsigned char)item[i])) {
                        match = false; break;
                    }
                }
                if (match) return true;
            }
        }
        if (*p == ',') p++;
    }
    return false;
}
static bool is_text_file(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext) {
        /* 无扩展名：查无后缀列表 */
        return list_contains(g_text_noext, name, false);
    }
    ext++;
    /* 有扩展名：查后缀列表 */
    return list_contains(g_text_exts, ext, false);
}
static bool is_text_file1(const char *name) {
    /* 无扩展名的已知文本文件 */
    if (!strcmp(name, "Makefile") || !strcmp(name, "makefile") ||
        !strcmp(name, "Dockerfile") || !strcmp(name, "CMakeLists.txt") ||
        !strcmp(name, "LICENSE") || !strcmp(name, "README") ||
        !strcmp(name, "CHANGELOG"))
        return true;
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    ext++;
    /* 常见文本扩展名 */
    static const char *text_exts[] = {
        "txt", "md", "markdown", "c", "h", "cpp", "hpp", "cc", "cxx",
        "py", "js", "ts", "jsx", "tsx", "json", "xml", "yaml", "yml",
        "html", "htm", "css", "scss", "less", "sh", "bash", "zsh",
        "ini", "conf", "cfg", "toml", "log", "csv", "tsv", "sql",
        "java", "go", "rs", "rb", "php", "pl", "lua", "r", "m",
        "swift", "kt", "scala", "cs", "fs", "vb", "pas", "asm",
        "cmake", "makefile", "dockerfile", "gitignore", "env",
        "tex", "rst", "adoc", "org", "properties", "gradle",
        NULL
    };
    for (int i = 0; text_exts[i]; i++)
        if (!strcasecmp(ext, text_exts[i])) return true;
    return false;
}


static void check_download(cui_ctx *ui) {
    if (!g_dl.active) return;

    char buf[256 * 1024];
    long long n = (long long)sftp_read(g_dl.file, buf, sizeof buf);
    if (n > 0) {
        if ((long long)fwrite(buf, 1, (size_t)n, g_dl.out) != n) {
            snprintf(g_dl.err, sizeof g_dl.err, "fwrite 失败");
            g_dl.active = false;
            fclose(g_dl.out);
            g_dl.out = NULL;
            sftp_close(g_dl.file);
            g_dl.file = NULL;
            g_transfers[g_dl.transfer_idx].status = XFER_FAILED;
            snprintf(g_transfers[g_dl.transfer_idx].err, sizeof g_transfers[g_dl.transfer_idx].err, "fwrite 失败");
            log_status_toast(ui, CUI_TONE_DANGER, T("下载失败：%s", "Download failed: %s"), g_dl.err);
            return;
        }
        g_dl.done_size += (uint64_t)n;
        g_transfers[g_dl.transfer_idx].done = g_dl.done_size;
        cui_request_wakeup(ui, 0.01f);   /* 下一帧继续 */
    } else if (n == 0) {
        /* 下载完成 */
        fclose(g_dl.out);
        g_dl.out = NULL;
        sftp_close(g_dl.file);
        g_dl.file = NULL;
        g_transfers[g_dl.transfer_idx].done = g_dl.done_size;
        g_transfers[g_dl.transfer_idx].status = XFER_DONE;
        log_status_toast(ui, CUI_TONE_SUCCESS,
                         T("下载完成：%s（%llu 字节）", "Downloaded: %s (%llu bytes)"),
                         g_dl.local_path, (unsigned long long)g_dl.done_size);
        g_dl.active = false;
        if (g_dl.open_after) {
            /* 双击触发：文本文件用外部编辑器，其他用默认程序 */
            bool is_text = is_text_file(g_dl.file_name);
            if (is_text && g_editor_path[0]) {
                /* 外部编辑器 */
#ifdef _WIN32
                HINSTANCE r = ShellExecuteA(NULL, "open", g_editor_path, g_dl.local_path, NULL, SW_SHOWNORMAL);
                if ((INT_PTR)r <= 32) {
                    ShellExecuteA(NULL, "open", g_dl.local_path, NULL, NULL, SW_SHOWNORMAL);
                }
#else
                char cmd[2048];
                snprintf(cmd, sizeof cmd, "%s \"%s\" &", g_editor_path, g_dl.local_path);
                system(cmd);
#endif
            } else {
                /* 默认程序 */
#ifdef _WIN32
                ShellExecuteA(NULL, "open", g_dl.local_path, NULL, NULL, SW_SHOWNORMAL);
#else
                char cmd[2048];
                snprintf(cmd, sizeof cmd, "xdg-open \"%s\" &", g_dl.local_path);
                system(cmd);
#endif
            }
        } else {
            /* 右键/按钮触发：只显示 toast */
            char title[256];
            snprintf(title, sizeof title, T("下载完成：%s", "Downloaded: %s"), g_dl.file_name);
            cui_toast(ui, title,
                      .description = g_dl.local_path,
                      .tone = CUI_TONE_SUCCESS,
                      .duration = 3.0f);
        }
        save_transfers();

        /* 注册监视：编辑后自动上传 */
        int found = -1;
        for (int i = 0; i < g_upload_watch_count; i++) {
            if (!strcmp(g_upload_watch[i].local_path, g_dl.local_path)) { found = i; break; }
        }
        if (found < 0 && g_upload_watch_count < MAX_UPLOAD_WATCH) {
            found = g_upload_watch_count++;
        }
        if (found >= 0) {
            upload_watch *w = &g_upload_watch[found];
            snprintf(w->local_path, sizeof w->local_path, "%s", g_dl.local_path);
            snprintf(w->remote_path, sizeof w->remote_path, "%s", g_dl.remote_path);
            w->tab_idx = g_dl.tab_idx;
            uint64_t m, s;
            if (file_stat(g_dl.local_path, &m, &s)) {
                w->last_mtime = m;
                w->last_size = s;
            }
            w->retries = 0;
            w->active = true;
        }
    } else {
        snprintf(g_dl.err, sizeof g_dl.err, "%s", ssh_get_error(g_tab_ssh[g_dl.tab_idx]));
        g_dl.active = false;
        fclose(g_dl.out);
        g_dl.out = NULL;
        sftp_close(g_dl.file);
        g_dl.file = NULL;
        g_transfers[g_dl.transfer_idx].status = XFER_FAILED;
        snprintf(g_transfers[g_dl.transfer_idx].err, sizeof g_transfers[g_dl.transfer_idx].err,
                 "%s", g_dl.err);
        log_status_toast(ui, CUI_TONE_DANGER, T("下载失败：%s", "Download failed: %s"), g_dl.err);
        save_transfers();
    }
}

/* 添加任务（环形，满了覆盖最旧） */
static int transfer_add(xfer_type type, const char *local, const char *remote,
                        uint64_t total, int tab_idx) {
    if (g_transfer_count >= MAX_TRANSFERS) {
        memmove(&g_transfers[0], &g_transfers[1],
                sizeof(g_transfers[0]) * (MAX_TRANSFERS - 1));
        g_transfer_count = MAX_TRANSFERS - 1;
    }
    int idx = g_transfer_count++;
    transfer_entry *t = &g_transfers[idx];
    memset(t, 0, sizeof *t);
    t->type = type;
    t->status = XFER_ACTIVE;
    snprintf(t->local, sizeof t->local, "%s", local);
    snprintf(t->remote, sizeof t->remote, "%s", remote);
    t->total = total;
    t->done = 0;
    t->tab_idx = tab_idx;
    t->start_time = time(NULL);
    save_transfers();
    return idx;
}

/* ================================================================== */
/* 下载 + 外部编辑器打开                                               */
/* ================================================================== */
static void download_and_open(cui_ctx *ui, const char *remote_path, const char *name, uint64_t size, bool open_after) {
    if (g_dl.active) {
        log_status_toast(ui, CUI_TONE_WARNING, T("已有下载进行中，稍后再试", "A download is already running"));
        return;
    }
    if (g_ssh_tab < 0 || g_ssh_tab >= g_tab_count) return;
    
    
    
    sftp_session sftp = g_tab_sftp[g_ssh_tab];
    if (!sftp) {
        log_status_toast(ui, CUI_TONE_DANGER, T("SFTP 未建立，无法下载", "SFTP not ready, cannot download"));
        return;
    }
    // if (size > 10ULL * 1024 * 1024) {
    //     log_status("文件过大（%.1f MB > 10 MB），未下载", size / (1024.0 * 1024.0));
    //     return;
    // }
    if (g_download_dir[0] == 0) {
        log_status_toast(ui, CUI_TONE_WARNING, T("未设置下载目录", "Download folder not set"));
        return;
    }
#ifdef _WIN32
    CreateDirectoryA(g_download_dir, NULL);
#endif

    char local[1024];
#ifdef _WIN32
    snprintf(local, sizeof local, "%s\\%s", g_download_dir, name);
#else
    snprintf(local, sizeof local, "%s/%s", g_download_dir, name);
#endif
    /* 下载前检查本地磁盘空间 */
    uint64_t need = size + 1024 * 1024 * 50;   /* 文件大小 + 50MB 余量 */
    uint64_t have = local_free_space(g_download_dir);
    if (have > 0 && have < need) {
        log_status_toast(ui, CUI_TONE_DANGER, T("本地磁盘空间不足：需要 %.1f MB，剩余 %.1f MB", "Not enough local space: need %.1f MB, have %.1f MB"),
                   need / (1024.0 * 1024.0), have / (1024.0 * 1024.0));
        int tidx = transfer_add(XFER_DOWNLOAD, local, remote_path, size, g_ssh_tab);
        g_transfers[tidx].status = XFER_FAILED;
        snprintf(g_transfers[tidx].err, sizeof g_transfers[tidx].err,
                 "本地磁盘空间不足");
        return;
    }
    /* 打开远程文件 */
    sftp_file f = sftp_open(sftp, remote_path, O_RDONLY, 0);
    if (!f) {
        log_status_toast(ui, CUI_TONE_DANGER, T("sftp_open 失败：%s", "sftp_open failed: %s"), ssh_get_error(g_tab_ssh[g_ssh_tab]));
        int tidx = transfer_add(XFER_DOWNLOAD, local, remote_path, size, g_ssh_tab);
        g_transfers[tidx].status = XFER_FAILED;
        snprintf(g_transfers[tidx].err, sizeof g_transfers[tidx].err, "%s", ssh_get_error(g_tab_ssh[g_ssh_tab]));
        return;
    }
    FILE *out = fopen(local, "wb");
    if (!out) {
        log_status_toast(ui, CUI_TONE_DANGER, T("无法创建本地文件：%s", "Cannot create local file: %s"), local);
        sftp_close(f);
        int tidx = transfer_add(XFER_DOWNLOAD, local, remote_path, size, g_ssh_tab);
        g_transfers[tidx].status = XFER_FAILED;
        snprintf(g_transfers[tidx].err, sizeof g_transfers[tidx].err, "无法创建本地文件");
        return;
    }

    /* 记录任务 */
    int tidx = transfer_add(XFER_DOWNLOAD, local, remote_path, size, g_ssh_tab);

    /* 启动分块下载状态机 */
    memset(&g_dl, 0, sizeof g_dl);
    g_dl.active = true;
    g_dl.transfer_idx = tidx;
    g_dl.tab_idx = g_ssh_tab;
    g_dl.file = f;
    g_dl.out = out;
    g_dl.total_size = size;
    g_dl.done_size = 0;
    g_dl.open_after = open_after;
    snprintf(g_dl.remote_path, sizeof g_dl.remote_path, "%s", remote_path);
    snprintf(g_dl.local_path, sizeof g_dl.local_path, "%s", local);
    snprintf(g_dl.file_name, sizeof g_dl.file_name, "%s", name);

    log_status(T("开始下载 %s（%.1f KB）", "Downloading %s (%.1f KB)"), remote_path, size / 1024.0);
    char title[256];
    snprintf(title, sizeof title, T("正在下载：%s", "Downloading: %s"), name);
    cui_toast(ui, title,
              .description = remote_path,
              .tone = CUI_TONE_INFO,
              .duration = 2.0f);
    cui_request_wakeup(ui, 0.01f);
    cui_request_wakeup(ui, 0.01f);   /* 下一帧继续 */
}

static bool upload_to_remote(cui_ctx *ui, int tab_idx, const char *local_path, const char *remote_path) {
    if (g_ul.active) {
        log_status_toast(ui, CUI_TONE_WARNING, T("已有上传进行中，稍后再试", "An upload is already running"));
        return false;
    }
    if (tab_idx < 0 || tab_idx >= g_tab_count) return false;
    sftp_session sftp = g_tab_sftp[tab_idx];
    if (!sftp) {
        log_status_toast(ui, CUI_TONE_DANGER, T("SFTP 未建立，无法上传", "SFTP not ready, cannot upload"));
        return false;
    }

    /* 拿本地文件大小 */
    uint64_t total_size = 0;
    FILE *in = fopen(local_path, "rb");
    if (!in) {
        log_status_toast(ui, CUI_TONE_DANGER, T("无法打开本地文件：%s", "Cannot open local file: %s"), local_path);
        return false;
    }
    fseek(in, 0, SEEK_END);
    total_size = (uint64_t)ftell(in);
    fseek(in, 0, SEEK_SET);

    /* 上传前检查远程目录磁盘空间（内部已调 remote_free_space） */
    if (!check_remote_space_for_upload(ui, tab_idx, remote_path, total_size)) {
        fclose(in);
        return false;
    }

    /* 打开远程文件 */
    sftp_file f = sftp_open(sftp, remote_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!f) {
        log_status_toast(ui, CUI_TONE_DANGER, T("远程写失败：%s", "remote (write) failed: %s"), ssh_get_error(g_tab_ssh[tab_idx]));
        fclose(in);
        int tidx = transfer_add(XFER_UPLOAD, local_path, remote_path, total_size, tab_idx);
        g_transfers[tidx].status = XFER_FAILED;
        snprintf(g_transfers[tidx].err, sizeof g_transfers[tidx].err,
                 "%s", ssh_get_error(g_tab_ssh[tab_idx]));
        save_transfers();
        return false;
    }

    int tidx = transfer_add(XFER_UPLOAD, local_path, remote_path, total_size, tab_idx);

    memset(&g_ul, 0, sizeof g_ul);
    g_ul.active = true;
    g_ul.transfer_idx = tidx;
    g_ul.tab_idx = tab_idx;
    g_ul.in = in;
    g_ul.file = f;
    g_ul.total_size = total_size;
    g_ul.done_size = 0;
    snprintf(g_ul.local_path, sizeof g_ul.local_path, "%s", local_path);
    snprintf(g_ul.remote_path, sizeof g_ul.remote_path, "%s", remote_path);

    log_status(T("开始上传 %s（%.1f KB）", "Uploading %s (%.1f KB)"), local_path, total_size / 1024.0);
    if (!g_upload_dir_active) {
        char title[256];
        const char *base = strrchr(local_path, '\\');
        if (!base) base = strrchr(local_path, '/');
        base = base ? base + 1 : local_path;
        snprintf(title, sizeof title, T("正在上传：%s", "Uploading: %s"), base);
        cui_toast(ui, title,
                  .description = remote_path,
                  .tone = CUI_TONE_INFO,
                  .duration = 2.0f);
    }
    return true;
}

static void check_uploads(cui_ctx *ui) {
    float now = (float)cui_time(ui);
    if (now - g_last_upload_check < 0.5f) return;
    g_last_upload_check = now;
    for (int i = 0; i < g_upload_watch_count; i++) {
        upload_watch *w = &g_upload_watch[i];
        if (!w->active) continue;
        uint64_t m, s;
        if (!file_stat(w->local_path, &m, &s)) continue;
        if (m != w->last_mtime || s != w->last_size) {
            log_status(T("检测到编辑：%s", "Edit detected: %s"), w->local_path);
            if (upload_to_remote(ui, w->tab_idx, w->local_path, w->remote_path)) {
                w->last_mtime = m; w->last_size = s; w->retries = 0;
            } else {
                w->retries++;
                if (w->retries > 5) { 
                    w->active = false; 
                    log_status_toast(ui, CUI_TONE_WARNING, T("上传失败多次，停止监视", "Upload failed repeatedly, stopped watching"));
                }
            }
        }
    }
}

static void check_upload(cui_ctx *ui) {
    if (!g_ul.active) return;

    char buf[64 * 1024];
    size_t n = fread(buf, 1, sizeof buf, g_ul.in);
    if (n > 0) {
        if (sftp_write(g_ul.file, buf, n) != (ssize_t)n) {
            snprintf(g_ul.err, sizeof g_ul.err, "%s", ssh_get_error(g_tab_ssh[g_ul.tab_idx]));
            g_ul.active = false;
            fclose(g_ul.in);
            g_ul.in = NULL;
            sftp_close(g_ul.file);
            g_ul.file = NULL;
            g_transfers[g_ul.transfer_idx].status = XFER_FAILED;
            snprintf(g_transfers[g_ul.transfer_idx].err, sizeof g_transfers[g_ul.transfer_idx].err,
                     "%s", g_ul.err);
            save_transfers();
            log_status_toast(ui, CUI_TONE_DANGER, T("上传失败：%s", "Upload failed: %s"), g_ul.err);
            return;
        }
        g_ul.done_size += n;
        g_transfers[g_ul.transfer_idx].done = g_ul.done_size;
        cui_request_wakeup(ui, 0.01f);
    } else {
        /* 上传完成 */
        fclose(g_ul.in);
        g_ul.in = NULL;
        sftp_close(g_ul.file);
        g_ul.file = NULL;
        g_transfers[g_ul.transfer_idx].done = g_ul.done_size;
        g_transfers[g_ul.transfer_idx].status = XFER_DONE;
        save_transfers();
        log_status_toast(ui, CUI_TONE_SUCCESS, T("上传完成：%s → %s（%llu 字节）", "Uploaded: %s → %s (%llu bytes)"),
                   g_ul.local_path, g_ul.remote_path, (unsigned long long)g_ul.done_size);
        g_ul.active = false;
        
        /* 检查是否有文件夹上传队列 */
        if (g_upload_dir_active) {
            g_upload_dir_file_count_done++;
            /* 启动下一个文件：失败则跳过，连续失败或队列空则中止 */
            bool started = false;
            while (!started && g_upload_dir_next_idx < g_upload_dir_file_count_total) {
                int k = g_upload_dir_next_idx++;
                if (upload_to_remote(ui, g_ssh_tab,
                                     g_upload_dir_files[k], g_upload_dir_remotes[k])) {
                    started = true;
                } else {
                    /* 单个文件失败：记日志，继续尝试下一个 */
                    log_status(T("文件夹上传跳过：%s", "Skipped: %s"), g_upload_dir_files[k]);
                }
            }
            if (!started) {
                /* 队列已空或全部失败：结束 */
                // char title[256];
                // snprintf(title, sizeof title,
                //          T("文件夹上传结束：%s（%d/%d 个文件）",
                //            "Folder upload finished: %s (%d/%d files)"),
                //          g_upload_dir_name,
                //          g_upload_dir_file_count_done,
                //          g_upload_dir_file_count_total);
                // cui_toast(ui, title,
                //           .description = g_upload_dir_remote,
                //           .tone = CUI_TONE_SUCCESS,
                //           .duration = 3.0f);
                g_upload_dir_active = false;
                if (g_ssh_tab >= 0 && g_ssh_tab < g_tab_count && g_tab_sftp[g_ssh_tab])
                    scan_remote(g_path);
            }
        } else {
            /* 单文件上传完成 toast */
            // char title[256];
            // const char *base = strrchr(g_ul.local_path, '\\');
            // if (!base) base = strrchr(g_ul.local_path, '/');
            // base = base ? base + 1 : g_ul.local_path;
            // snprintf(title, sizeof title, T("上传完成：%s", "Uploaded: %s"), base);
            // cui_toast(ui, title,
            //           .description = g_ul.remote_path,
            //           .tone = CUI_TONE_SUCCESS,
            //           .duration = 3.0f);
            if (g_ssh_tab >= 0 && g_ssh_tab < g_tab_count && g_tab_sftp[g_ssh_tab])
                scan_remote(g_path);
        }
    }
}

/* 同步上传单个文件（供文件夹递归用） */
/* 同步上传单个文件（阻塞，供需要"上传完再继续"的场景用） */
static bool upload_file_sync(cui_ctx *ui, int tab_idx, const char *local_path, const char *remote_path) {
    if (tab_idx < 0 || tab_idx >= g_tab_count) return false;
    sftp_session sftp = g_tab_sftp[tab_idx];
    if (!sftp) {
        log_status_toast(ui, CUI_TONE_DANGER, T("SFTP 未建立，无法上传", "SFTP not ready, cannot upload"));
        return false;
    }

    FILE *in = fopen(local_path, "rb");
    if (!in) {
        log_status_toast(ui, CUI_TONE_DANGER, T("无法打开本地文件：%s", "Cannot open local file: %s"), local_path);
        return false;
    }
    fseek(in, 0, SEEK_END);
    uint64_t total_size = (uint64_t)ftell(in);
    fseek(in, 0, SEEK_SET);

    /* 上传前检查远程空间 */
    if (!check_remote_space_for_upload(ui, tab_idx, remote_path, total_size)) {
        fclose(in);
        return false;
    }

    sftp_file f = sftp_open(sftp, remote_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!f) {
        log_status_toast(ui, CUI_TONE_DANGER, T("远程写 失败：%s", "remote (write) failed: %s"), ssh_get_error(g_tab_ssh[tab_idx]));
        fclose(in);
        int tidx = transfer_add(XFER_UPLOAD, local_path, remote_path, total_size, tab_idx);
        g_transfers[tidx].status = XFER_FAILED;
        snprintf(g_transfers[tidx].err, sizeof g_transfers[tidx].err,
                 "%s", ssh_get_error(g_tab_ssh[tab_idx]));
        save_transfers();
        return false;
    }

    int tidx = transfer_add(XFER_UPLOAD, local_path, remote_path, total_size, tab_idx);

    char buf[8192];
    size_t n;
    uint64_t total = 0;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (sftp_write(f, buf, n) != (ssize_t)n) {
            snprintf(g_transfers[tidx].err, sizeof g_transfers[tidx].err,
                     "%s", ssh_get_error(g_tab_ssh[tab_idx]));
            ok = false;
            break;
        }
        total += n;
        g_transfers[tidx].done = total;
    }
    fclose(in);
    sftp_close(f);
    if (ok) {
        g_transfers[tidx].done = total;
        g_transfers[tidx].status = XFER_DONE;
        save_transfers();
        log_status_toast(ui, CUI_TONE_SUCCESS, T("已上传 %s → %s（%llu 字节）", "Uploaded: %s → %s (%llu bytes)"), local_path, remote_path,
                   (unsigned long long)total);
    } else {
        g_transfers[tidx].status = XFER_FAILED;
        save_transfers();
        log_status_toast(ui, CUI_TONE_DANGER, T("上传失败：%s", "Upload failed: %s"), g_transfers[tidx].err);
    }
    return ok;
}

/* ================================================================== */
/* INI                                                                 */
/* ================================================================== */

static void ini_path(char *out, size_t cap) {
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata) {
        snprintf(out, cap, "%s\\CUI\\sessions.ini", appdata);
        char dir[MAX_PATH];
        snprintf(dir, sizeof dir, "%s\\CUI", appdata);
        CreateDirectoryA(dir, NULL);
    } else snprintf(out, cap, "sessions.ini");
#else
    snprintf(out, cap, "sessions.ini");
#endif
}

static void session_nav_add(int sess_idx, const char *path) {
    if (sess_idx < 0 || sess_idx >= g_session_count) return;
    if (!path || !*path) return;
    ssh_session_cfg *s = &g_sessions[sess_idx];
    /* 已存在：移到最前 */
    for (int i = 0; i < s->nav_count; i++) {
        if (!strcmp(s->nav_history[i], path)) {
            for (int j = i; j > 0; j--)
                memcpy(s->nav_history[j], s->nav_history[j-1], sizeof s->nav_history[0]);
            snprintf(s->nav_history[0], sizeof s->nav_history[0], "%s", path);
            return;
        }
    }
    /* 新路径：插入最前，满了丢最后 */
    if (s->nav_count < 20) {
        for (int j = s->nav_count; j > 0; j--)
            memcpy(s->nav_history[j], s->nav_history[j-1], sizeof s->nav_history[0]);
        s->nav_count++;
    } else {
        for (int j = 19; j > 0; j--)
            memcpy(s->nav_history[j], s->nav_history[j-1], sizeof s->nav_history[0]);
    }
    snprintf(s->nav_history[0], sizeof s->nav_history[0], "%s", path);
}

static void save_sessions(void) {
    char path[512]; ini_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "[settings]\n");
    fprintf(f, "download_dir=%s\n", g_download_dir);
    fprintf(f, "editor=%s\n", g_editor_path);
    fprintf(f, "ssh_key_path=%s\n", g_ssh_key_path);
    fprintf(f, "text_noext=%s\n", g_text_noext);
    fprintf(f, "text_exts=%s\n", g_text_exts);
    fprintf(f, "term_font_size=%.1f\n", g_term_font_size);
    fprintf(f, "dark=%d\n", g_dark ? 1 : 0);
    fprintf(f, "lang=%c\n", lang);
    fprintf(f, "win_w=%d\n", g_win_w);
    fprintf(f, "win_h=%d\n", g_win_h);
    fprintf(f, "split_main=%.4f\n", g_split_ratio);
    fprintf(f, "split_left=%.4f\n", g_left_v_split);
    fprintf(f, "split_right=%.4f\n", g_v_split);
    fprintf(f, "split_file=%.4f\n", g_file_split);
    fprintf(f, "show_sidebar=%d\n", g_show_sidebar ? 1 : 0);
    fprintf(f, "show_fm_panel=%d\n", g_show_fm_panel ? 1 : 0);
    fprintf(f, "\n");

    for (int i = 0; i < g_session_count; i++) {
        ssh_session_cfg *s = &g_sessions[i];
        fprintf(f, "[%s]\n", s->name);
        fprintf(f, "host=%s\n", s->host);
        fprintf(f, "port=%d\n", s->port);
        fprintf(f, "user=%s\n", s->user);
        fprintf(f, "pass=%s\n", s->pass_enc);        
        fprintf(f, "nav_count=%d\n", s->nav_count);
        for (int k = 0; k < s->nav_count; k++)
            fprintf(f, "nav_%d=%s\n", k, s->nav_history[k]);
        fprintf(f, "fwd_count=%d\n", s->fwd_count);                    /* ← 新增 */
        for (int k = 0; k < s->fwd_count; k++) {                       /* ← 新增 */
            forward_cfg *fc = &s->fwd[k];
            fprintf(f, "fwd_%d_active=%d\n", k, fc->active ? 1 : 0);
            fprintf(f, "fwd_%d_local_host=%s\n", k, fc->local_host);
            fprintf(f, "fwd_%d_local_port=%d\n", k, fc->local_port);
            fprintf(f, "fwd_%d_remote_host=%s\n", k, fc->remote_host);
            fprintf(f, "fwd_%d_remote_port=%d\n", k, fc->remote_port);
        }
        fprintf(f, "\n");
    }
    fclose(f);
}

static void load_sessions(void) {
    char path[512]; ini_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    g_session_count = 0;
    ssh_session_cfg *cur = NULL;
    bool in_settings = false;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == ';' || *p == '#') continue;
        if (*p == '[') {
            char *end = strchr(p, ']');
            if (end) *end = 0;
            if (!strcmp(p+1, "settings")) { in_settings = true; cur = NULL; continue; }
            in_settings = false;
            if (g_session_count >= MAX_SESSIONS) break;
            cur = &g_sessions[g_session_count++];
            memset(cur, 0, sizeof *cur);
            snprintf(cur->name, sizeof cur->name, "%s", p+1);
            cur->port = 22;
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = p, *val = eq+1;
        char *nl = strchr(val, '\n'); if (nl) *nl = 0;
        if (in_settings) {
            if (!strcmp(key, "download_dir")) snprintf(g_download_dir, sizeof g_download_dir, "%s", val);
            else if (!strcmp(key, "editor")) snprintf(g_editor_path, sizeof g_editor_path, "%s", val);
            else if (!strcmp(key, "ssh_key_path")) snprintf(g_ssh_key_path, sizeof g_ssh_key_path, "%s", val);   /* ← 新增 */
            else if (!strcmp(key, "text_noext")) snprintf(g_text_noext, sizeof g_text_noext, "%s", val);
            else if (!strcmp(key, "text_exts")) snprintf(g_text_exts, sizeof g_text_exts, "%s", val);
            else if (!strcmp(key, "term_font_size")) g_term_font_size = (float)atof(val);
            else if (!strcmp(key, "dark")) g_dark = atoi(val) != 0;
            else if (!strcmp(key, "lang") && (val[0] == 'z' || val[0] == 'e')) lang = val[0];
            else if (!strcmp(key, "win_w")) { int v = atoi(val); if (v > 200 && v < 10000) g_win_w = v; }
            else if (!strcmp(key, "win_h")) { int v = atoi(val); if (v > 200 && v < 10000) g_win_h = v; }
            else if (!strcmp(key, "split_main")) { float v = (float)atof(val); if (v > 0.05f && v < 0.95f) g_split_ratio = v; }
            else if (!strcmp(key, "split_left")) { float v = (float)atof(val); if (v > 0.05f && v < 0.95f) g_left_v_split = v; }
            else if (!strcmp(key, "split_right")) { float v = (float)atof(val); if (v > 0.05f && v < 0.95f) g_v_split = v; }
            else if (!strcmp(key, "split_file")) { float v = (float)atof(val); if (v > 0.05f && v < 0.95f) g_file_split = v; }
            else if (!strcmp(key, "show_sidebar")) g_show_sidebar = atoi(val) != 0;
            else if (!strcmp(key, "show_fm_panel")) g_show_fm_panel = atoi(val) != 0;
        } else if (cur) {
            if (!strcmp(key, "host"))  snprintf(cur->host, sizeof cur->host, "%s", val);
            else if (!strcmp(key, "port")) cur->port = atoi(val);
            else if (!strcmp(key, "user")) snprintf(cur->user, sizeof cur->user, "%s", val);
            else if (!strcmp(key, "pass")) snprintf(cur->pass_enc, sizeof cur->pass_enc, "%s", val);
            else if (!strcmp(key, "nav_count")) cur->nav_count = atoi(val);
            else if (!strncmp(key, "nav_", 4)) {
                int k = atoi(key + 4);
                if (k >= 0 && k < 20) {
                    snprintf(cur->nav_history[k], sizeof cur->nav_history[k], "%s", val);
                    if (k + 1 > cur->nav_count) cur->nav_count = k + 1;
                }
            }
            /* ← 新增 */
            else if (!strcmp(key, "fwd_count")) cur->fwd_count = atoi(val);
            else if (!strncmp(key, "fwd_", 4)) {
                int k = 0, off = 0;
                if (sscanf(key + 4, "%d_%n", &k, &off) == 1 && k >= 0 && k < MAX_FWD_CFG) {
                    if (k + 1 > cur->fwd_count) cur->fwd_count = k + 1;
                    forward_cfg *fc = &cur->fwd[k];
                    const char *field = key + 4 + off;
                    if (!strcmp(field, "active")) fc->active = atoi(val) != 0;
                    else if (!strcmp(field, "local_host")) snprintf(fc->local_host, sizeof fc->local_host, "%s", val);
                    else if (!strcmp(field, "local_port")) fc->local_port = atoi(val);
                    else if (!strcmp(field, "remote_host")) snprintf(fc->remote_host, sizeof fc->remote_host, "%s", val);
                    else if (!strcmp(field, "remote_port")) fc->remote_port = atoi(val);
                }
            }
        }
    }
    fclose(f);
    if (g_download_dir[0] == 0) default_download_dir(g_download_dir, sizeof g_download_dir);
}

/* ================================================================== */
/* SSH / SFTP                                                          */
/* ================================================================== */

static bool do_connect(ssh_session *out, const char *host, int port,
                       const char *user, const char *pass,
                       char *errbuf, size_t errcap) {
    log_status(T("连接 %s:%d，用户 %s ...", "Connecting %s:%d as %s ..."), host, port, user);
    if (out && *out) { ssh_disconnect(*out); ssh_free(*out); *out = NULL; }
    ssh_session s = ssh_new();
    if (!s) { if (errbuf) snprintf(errbuf, errcap, "ssh_new failed"); return false; }
    long p = port;
    ssh_options_set(s, SSH_OPTIONS_HOST, host);
    ssh_options_set(s, SSH_OPTIONS_PORT, &p);
    ssh_options_set(s, SSH_OPTIONS_USER, user);

    int rc = ssh_connect(s);
    if (rc != SSH_OK) {
        log_status(T("连接失败：%s", "Connect failed: %s"), ssh_get_error(s));
        if (errbuf) snprintf(errbuf, errcap, "connect: %s", ssh_get_error(s));
        ssh_free(s); return false;
    }
    
    bool key_tried = false;
    if (g_ssh_key_path[0]) {
        ssh_key privkey = NULL;
        int irc = ssh_pki_import_privkey_file(g_ssh_key_path, NULL, NULL, NULL, &privkey);
        if (irc == SSH_OK && privkey) {
            key_tried = true;
            int arc = ssh_userauth_publickey(s, NULL, privkey);
            ssh_key_free(privkey);
            if (arc == SSH_AUTH_SUCCESS) {
                log_status(T("已用私钥连接 %s@%s:%d（%s）", "Connected with key %s@%s:%d (%s)"), user, host, port, g_ssh_key_path);
                if (out) *out = s;
                return true;
            }
            log_status(T("私钥认证失败，回退密码：%s", "Key auth failed, falling back to password: %s"), ssh_get_error(s));
        } else {
           log_status(T("无法读取私钥 %s：%s，回退密码", "Cannot read key %s: %s, falling back to password"), g_ssh_key_path, ssh_get_error(s));
        }
    }
    
    rc = ssh_userauth_password(s, NULL, pass);
    if (rc != SSH_AUTH_SUCCESS) {
        if (key_tried)
            log_status(T("认证失败（私钥与密码均失败）：%s", "Auth failed (key and password): %s"), ssh_get_error(s));
        else
            log_status(T("认证失败：%s", "Auth failed: %s"), ssh_get_error(s));
        if (errbuf) snprintf(errbuf, errcap, "auth: %s", ssh_get_error(s));
        ssh_disconnect(s); ssh_free(s); return false;
    }
    log_status(T("已连接 %s@%s:%d（密码）", "Connected %s@%s:%d (password)"), user, host, port);
    if (out) *out = s;
    return true;
}

static bool exec_remote(ssh_session s, const char *cmd, char *out, size_t cap) {
    if (!s) return false;
    ssh_channel ch = ssh_channel_new(s);
    if (!ch) return false;
    if (ssh_channel_open_session(ch) != SSH_OK) { ssh_channel_free(ch); return false; }
    if (ssh_channel_request_exec(ch, cmd) != SSH_OK) { ssh_channel_free(ch); return false; }
    size_t n = 0; int waited_ms = 0;
    while (n < cap - 1 && waited_ms < 3000) {
        int r = ssh_channel_read_nonblocking(ch, out + n, (uint32_t)(cap - n - 1), 0);
        if (r > 0) { n += (size_t)r; continue; }
        if (r == 0) {
            if (ssh_channel_is_eof(ch)) break;
#ifdef _WIN32
            Sleep(10);
#else
            usleep(10000);
#endif
            waited_ms += 10; continue;
        }
        break;
    }
    out[n] = 0;
    ssh_channel_send_eof(ch); ssh_channel_close(ch); ssh_channel_free(ch);
    return true;
}

static void refresh_stats(void) {
    g_cpu = 20.0f + (float)(rand() % 3000) / 100.0f;
    g_ram = 40.0f + (float)(rand() % 2000) / 100.0f;
    g_swap = 5.0f + (float)(rand() % 1000) / 100.0f;
    g_load[0] = (float)(rand() % 100) / 100.0f;
    g_load[1] = (float)(rand() % 80) / 100.0f;
    g_load[2] = (float)(rand() % 60) / 100.0f;
    g_net_rx = (float)(rand() % 5000) / 1000.0f;
    g_net_tx = (float)(rand() % 3000) / 1000.0f;
    g_rx_hist[g_net_head] = g_net_rx;
    g_tx_hist[g_net_head] = g_net_tx;
    g_net_head = (g_net_head + 1) % NET_HIST;
}

static void refresh_stats_real(int sess_idx) {
    (void)sess_idx;
    ssh_session s = g_tab_ssh[g_ssh_tab];
    if (!s) return;
    char buf[4096] = "";
    if (!exec_remote(s,
        "echo '---LOAD---'; cat /proc/loadavg; "
        "echo '---UPTIME---'; cat /proc/uptime; "
        "echo '---MEM---'; grep -E '^(MemTotal|MemAvailable|SwapTotal|SwapFree):' /proc/meminfo; "
        "echo '---CPU---'; grep '^cpu ' /proc/stat; "
        "echo '---NET---'; cat /proc/net/dev; "
        "echo '---DF---'; df -P -h | tail -n +2; "
        "echo '---PROC---'; ps -eo comm,%cpu,%mem --sort=-%mem | head -5; "
        "echo '---END---'",
        buf, sizeof buf)) return;

    char *p;
    if ((p = strstr(buf, "---LOAD---"))) {
        float a = 0, b = 0, c = 0;
        if (sscanf(p + 10, " %f %f %f", &a, &b, &c) == 3) {
            g_load[0] = a; g_load[1] = b; g_load[2] = c;
        }
    }
    if ((p = strstr(buf, "---UPTIME---"))) {
        double up = 0;
        if (sscanf(p + 12, " %lf", &up) == 1) g_uptime_sec = (unsigned long long)up;
    }
    if ((p = strstr(buf, "---MEM---"))) {
        char *line = p;
        unsigned long long mem_total = 0, mem_avail = 0, swap_total_kb = 0, swap_free_kb = 0;
        while ((line = strchr(line, '\n')) != NULL) {
            line++;
            unsigned long long v = 0;
            if (sscanf(line, "MemTotal: %llu", &v) == 1)        mem_total = v * 1024;
            else if (sscanf(line, "MemAvailable: %llu", &v) == 1) mem_avail = v * 1024;
            else if (sscanf(line, "SwapTotal: %llu", &v) == 1)    swap_total_kb = v;
            else if (sscanf(line, "SwapFree: %llu", &v) == 1)     swap_free_kb = v;
            if (strncmp(line, "---CPU---", 9) == 0) break;
        }
        if (mem_total > 0) {
            g_ram_total = mem_total;
            g_ram_used = mem_total - mem_avail;
            g_ram = (float)((double)g_ram_used / (double)mem_total * 100.0);
        }
        if (swap_total_kb > 0) {
            g_swap_total = swap_total_kb * 1024;
            g_swap_used = (swap_total_kb - swap_free_kb) * 1024;
            g_swap = (float)((double)g_swap_used / (double)g_swap_total * 100.0);
        } else { g_swap_total = g_swap_used = 0; g_swap = 0; }
    }
    if ((p = strstr(buf, "---CPU---"))) {
        char *nl = strchr(p, '\n');
        if (nl) {
            unsigned long long user=0, nice=0, sys=0, idle=0, iow=0, irq=0, sirq=0, steal=0;
            if (sscanf(nl + 1, "cpu  %llu %llu %llu %llu %llu %llu %llu %llu",
                       &user, &nice, &sys, &idle, &iow, &irq, &sirq, &steal) >= 4) {
                unsigned long long total = user + nice + sys + idle + iow + irq + sirq + steal;
                if (g_stat_last_cpu_total > 0 && total > g_stat_last_cpu_total) {
                    unsigned long long dtotal = total - g_stat_last_cpu_total;
                    unsigned long long didle  = idle  - g_stat_last_cpu_idle;
                    if (dtotal > 0) g_cpu = (float)((double)(dtotal - didle) / (double)dtotal * 100.0);
                }
                g_stat_last_cpu_total = total;
                g_stat_last_cpu_idle  = idle;
            }
        }
    }
    if ((p = strstr(buf, "---NET---"))) {
        unsigned long long rx = 0, tx = 0;
        char *line = p;
        while ((line = strchr(line, '\n')) != NULL) {
            line++;
            char iface[32];
            unsigned long long rxb = 0, txb = 0;
            if (sscanf(line, " %31[^:]: %llu %*u %*u %*u %*u %*u %*u %*u %llu",
                       iface, &rxb, &txb) == 3) {
                if (strncmp(iface, "eth", 3) == 0 || strncmp(iface, "ens", 3) == 0 ||
                    strncmp(iface, "enp", 3) == 0) { rx += rxb; tx += txb; }
            }
            if (strncmp(line, "---DF---", 8) == 0) break;
        }
        time_t now = time(NULL);
        if (g_stat_last_time > 0 && now > (time_t)g_stat_last_time) {
            double dt = (double)(now - (time_t)g_stat_last_time);
            if (rx >= g_stat_last_rx_bytes) g_net_rx = (float)((double)(rx - g_stat_last_rx_bytes) / dt / 1e6);
            if (tx >= g_stat_last_tx_bytes) g_net_tx = (float)((double)(tx - g_stat_last_tx_bytes) / dt / 1e6);
        }
        g_stat_last_rx_bytes = rx; g_stat_last_tx_bytes = tx; g_stat_last_time = (float)now;
        g_rx_hist[g_net_head] = g_net_rx; g_tx_hist[g_net_head] = g_net_tx;
        g_net_head = (g_net_head + 1) % NET_HIST;
    }
    if ((p = strstr(buf, "---DF---"))) {
        g_dir_count = 0;
        char *line = p + 8;
        while (line && *line && g_dir_count < 64) {
            char *next = strchr(line, '\n');
            if (!next) break;
            *next = 0;
            char fs[64], size[32], used[32], avail[32], pct[16], mnt[256];
            if (sscanf(line, "%63s %31s %31s %31s %15s %255s",
                       fs, size, used, avail, pct, mnt) == 6) {
                snprintf(g_dirs[g_dir_count].path, sizeof g_dirs[g_dir_count].path, "%s", mnt);
                snprintf(g_dirs[g_dir_count].used, sizeof g_dirs[g_dir_count].used, "%s", used);
                snprintf(g_dirs[g_dir_count].total, sizeof g_dirs[g_dir_count].total, "%s", size);
                g_dir_count++;
            }
            *next = '\n';
            line = next + 1;
            if (strncmp(line, "---PROC---", 10) == 0) break;
        }
    }
    if ((p = strstr(buf, "---PROC---"))) {
        int k = 0;
        char *line = p;
        while (k < PROC_COUNT && (line = strchr(line, '\n')) != NULL) {
            line++;
            char name[64] = "";
            float cpu = 0, mem = 0;
            if (sscanf(line, "%63s %f %f", name, &cpu, &mem) == 3) {
                if (strcmp(name, "COMMAND") == 0) continue;
                if (g_procs[k].name) free(g_procs[k].name);
                g_procs[k].name = strdup(name);
                g_procs[k].cmd = g_procs[k].name;
                g_procs[k].cpu = cpu;
                g_procs[k].mem = mem;
                k++;
            }
            if (strncmp(line, "---END---", 9) == 0) break;
        }
        for (; k < PROC_COUNT; k++) {
            if (g_procs[k].name) { free(g_procs[k].name); g_procs[k].name = NULL; }
            g_procs[k].cmd = ""; g_procs[k].cpu = 0; g_procs[k].mem = 0;
        }
    }
}

/* ================================================================== */
/* 远程目录扫描（SFTP）+ 树加载                                        */
/* ================================================================== */

static void scan_remote(const char *path) {
    if (g_ssh_tab < 0 || g_ssh_tab >= g_tab_count) { 
        log_status(T("scan_remote: tab 越界", "scan_remote: tab out of range")); 
        return; 
    }
    sftp_session sftp = g_tab_sftp[g_ssh_tab];
    if (!sftp) { 
        log_status(T("scan_remote: sftp 为 NULL", "scan_remote: sftp is NULL")); 
        return; 
    }
    g_file_count = 0;
    g_file_selected = -1;
    snprintf(g_path, sizeof g_path, "%s", path);
    history_add(path);
    sftp_dir dir = sftp_opendir(sftp, path);
    if (!dir) {
        log_status(T("sftp_opendir(%s) 失败：%s", "sftp_opendir(%s) failed: %s"), path, ssh_get_error(g_tab_ssh[g_ssh_tab]));
        return;
    }
    sftp_attributes attr;
    int n = 0;
    while ((attr = sftp_readdir(sftp, dir)) != NULL) {
        if (!strcmp(attr->name, ".") || !strcmp(attr->name, "..")) {
            sftp_attributes_free(attr);
            continue;
        }
        if (g_file_count >= g_file_cap) {
            g_file_cap = g_file_cap ? g_file_cap * 2 : 128;
            g_files = (file_row*)realloc(g_files, sizeof(file_row) * g_file_cap);
        }
        file_row *r = &g_files[g_file_count++];
        memset(r, 0, sizeof *r);
        snprintf(r->name, sizeof r->name, "%s", attr->name);
        r->is_dir = (attr->type == SSH_FILEXFER_TYPE_DIRECTORY);
        r->bytes = (uint64_t)attr->size;
        if (r->is_dir) snprintf(r->size, sizeof r->size, "-");
        else if (attr->size < 1024) snprintf(r->size, sizeof r->size, "%lluB", (unsigned long long)attr->size);
        else if (attr->size < 1024*1024) snprintf(r->size, sizeof r->size, "%.1fK", attr->size/1024.0);
        else snprintf(r->size, sizeof r->size, "%.1fM", attr->size/(1024.0*1024.0));

        time_t mt = (time_t)attr->mtime;
        struct tm *tmv = localtime(&mt);
        if (tmv) strftime(r->mtime, sizeof r->mtime, "%Y-%m-%d", tmv);

        //snprintf(r->perm, sizeof r->perm, "%o", attr->permissions & 07777);
        format_perm(attr->permissions, r->is_dir, r->perm, sizeof r->perm);
        
        snprintf(r->owner, sizeof r->owner, "%s/%s",
                 attr->owner ? attr->owner : "-",
                 attr->group ? attr->group : "-");
        n++;
        sftp_attributes_free(attr);
    }
    sftp_closedir(dir);
    log_status(T("scan_remote(%s) 完成：%d 个文件", "scan_remote(%s) done: %d files"), path, n);
    if (g_ssh_tab >= 0 && g_ssh_tab < g_tab_count) {
        int sess = g_tabs[g_ssh_tab].session_idx;
        session_nav_add(sess, path);
    }
}

/* ================================================================== */
/* 树状态管理（递归展开）                                              */
/* ================================================================== */

static int tree_find(const char *path) {
    for (int i = 0; i < g_tree_count; i++)
        if (!strcmp(g_tree[i].path, path)) return i;
    return -1;
}

static int tree_add(const char *path, const char *name, int depth, bool open) {
    int i = tree_find(path);
    if (i >= 0) return i;
    if (g_tree_count >= 512) return -1;
    i = g_tree_count++;
    snprintf(g_tree[i].path, sizeof g_tree[i].path, "%s", path);
    snprintf(g_tree[i].name, sizeof g_tree[i].name, "%s", name);
    g_tree[i].depth = depth;
    g_tree[i].open = open;
    g_tree[i].loaded = false;
    return i;
}

/* 从 / 到 target 的祖先链都要在树里，且 open */
static void tree_ensure_path(const char *target) {
    tree_add("/", "/", 0, true);
    if (strcmp(target, "/") == 0) return;
    char cur[512] = "";
    const char *p = target;
    int depth = 1;
    while (*p) {
        if (*p == '/') p++;
        if (!*p) break;
        const char *end = strchr(p, '/');
        int len = end ? (int)(end - p) : (int)strlen(p);
        char segment[256];
        if (len >= (int)sizeof segment) len = sizeof segment - 1;
        memcpy(segment, p, len); segment[len] = 0;
        strcat(cur, "/"); strcat(cur, segment);
        int i = tree_find(cur);
        if (i < 0) i = tree_add(cur, segment, depth, true);
        else g_tree[i].open = true;
        p += len; depth++;
    }
}

static int cmp_str(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static void tree_load_children(const char *path) {
    if (g_ssh_tab < 0 || g_ssh_tab >= g_tab_count) return;
    sftp_session sftp = g_tab_sftp[g_ssh_tab];
    if (!sftp) return;
    int parent = tree_find(path);
    if (parent < 0) { 
        log_status(T("tree_load: 父级未找到 %s", "tree_load: parent not found %s"), path); 
        return; 
    }
    if (g_tree[parent].loaded) return;

    sftp_dir dir = sftp_opendir(sftp, path);
    if (!dir) { g_tree[parent].loaded = true; return; }

    char sub_names[256][256];
    int sub_count = 0;
    sftp_attributes attr;
    while ((attr = sftp_readdir(sftp, dir)) != NULL) {
        if (!strcmp(attr->name, ".") || !strcmp(attr->name, "..")) {
            sftp_attributes_free(attr);
            continue;
        }
        if (attr->type == SSH_FILEXFER_TYPE_DIRECTORY && sub_count < 256) {
            snprintf(sub_names[sub_count], sizeof sub_names[sub_count], "%s", attr->name);
            sub_count++;
        }
        sftp_attributes_free(attr);
    }
    sftp_closedir(dir);

    /* 按字母排序 */
    if (sub_count > 1)
        qsort(sub_names, sub_count, sizeof(sub_names[0]), cmp_str);

    if (sub_count > 0 && g_tree_count + sub_count <= 512) {
        int insert_at = parent + 1;
        memmove(&g_tree[insert_at + sub_count],
                &g_tree[insert_at],
                sizeof(tree_entry) * (g_tree_count - insert_at));
        g_tree_count += sub_count;
        for (int i = 0; i < sub_count; i++) {
            char full[512];
            if (strcmp(path, "/") == 0) snprintf(full, sizeof full, "/%s", sub_names[i]);
            else snprintf(full, sizeof full, "%s/%s", path, sub_names[i]);
            snprintf(g_tree[insert_at + i].path, sizeof g_tree[insert_at + i].path, "%s", full);
            snprintf(g_tree[insert_at + i].name, sizeof g_tree[insert_at + i].name, "%s", sub_names[i]);
            g_tree[insert_at + i].depth = g_tree[parent].depth + 1;
            g_tree[insert_at + i].open = false;
            g_tree[insert_at + i].loaded = false;
        }
    }
    g_tree[parent].loaded = true;
    log_status(T("tree_load(%s): 增加 %d 个子目录", "tree_load(%s): added %d subdirs"), path, sub_count);
}

/* ================================================================== */
/* 比较函数 / 排序                                                     */
/* ================================================================== */

static int file_cmp_ff(const void *pa, const void *pb, int col) {
    const file_row *a = pa, *b = pb;
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    switch (col) {
    case 0: return strcmp(a->name, b->name);
    case 1: return strcmp(a->size, b->size);
    case 2: return strcmp(a->mtime, b->mtime);
    case 3: return strcmp(a->perm, b->perm);
    case 4: return strcmp(a->owner, b->owner);
    }
    return 0;
}
static int dir_cmp(const void *a, const void *b, int col) {
    const dir_usage *ra = a, *rb = b;
    if (col == 0) return strcmp(ra->path, rb->path);
    if (col == 1) return strcmp(ra->used, rb->used);
    return 0;
}
static int proc_cmp(const void *a, const void *b, int col) {
    const proc_row *ra = a, *rb = b;
    if (col == 0) return strcmp(ra->cmd, rb->cmd);
    if (col == 1) { float d = ra->cpu - rb->cpu; return (d > 0) - (d < 0); }
    if (col == 2) { float d = ra->mem - rb->mem; return (d > 0) - (d < 0); }
    return 0;
}

static void sort_indices(int *idx, int n, cui_table_sort sort,
                         const void *base, size_t stride,
                         int (*cmp)(const void *a, const void *b, int col)) {
    for (int i = 0; i < n; i++) idx[i] = i;
    if (sort.column < 0) return;
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0; j--) {
            const void *a = (const char *)base + idx[j-1] * stride;
            const void *b = (const char *)base + idx[j]   * stride;
            int c = cmp(a, b, sort.column);
            if (sort.descending) c = -c;
            if (c > 0) { int t = idx[j]; idx[j] = idx[j-1]; idx[j-1] = t; }
            else break;
        }
    }
}

/* ================================================================== */
/* UI 辅助                                                             */
/* ================================================================== */

static void hline(cui_ctx *ui) {
    cui_box(ui, .width = CUI_FILL, .height = CUI_PX(1), .bg = CUI_COLOR_BORDER) {}
}

static void sortable_header(cui_ctx *ui, const char *table, const char *label, float width,
                             bool grow, cui_text_align align, cui_table_sort *sort, int column) {
    cui_size w = grow ? CUI_GROW(1) : CUI_PX(width);
    bool active = sort->column == column;
    const char *arrow = active ? (sort->descending ? CUI_ICON_CHEVRON_DOWN : CUI_ICON_CHEVRON_UP) : NULL;
    char id_str[64];
    snprintf(id_str, sizeof id_str, "%s-hdr-%d", table, column);
    cui_id hid = cui_make_id(ui, id_str);
    cui_box(ui, .width = w, .height = CUI_PX(24), .id = id_str) {
        cui_row(ui, .width = CUI_FILL, .gap = 4, .align = CUI_ALIGN_CENTER,
                .justify = (align == CUI_TEXT_CENTER ? CUI_JUSTIFY_CENTER :
                            align == CUI_TEXT_RIGHT  ? CUI_JUSTIFY_END : CUI_JUSTIFY_START)) {
            cui_text(ui, label, .size = 11, .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
            if (arrow) cui_icon(ui, arrow, 12, CUI_COLOR_TEXT_SECONDARY);
        }
    }
    cui_rect r = cui_get_rect(ui, hid);
    if (r.w > 0) {
        cui_vec2 m = cui_mouse_pos(ui);
        if (m.x >= r.x && m.x <= r.x + r.w && m.y >= r.y && m.y <= r.y + r.h
            && cui_mouse_clicked(ui, CUI_MOUSE_LEFT)) {
            if (sort->column == column) sort->descending = !sort->descending;
            else { sort->column = column; sort->descending = false; }
        }
    }
}

static double g_last_click_time[MAX_SESSIONS];
static int    g_last_clicked[MAX_SESSIONS];
static bool is_double_click(cui_ctx *ui, int idx, bool clicked) {
    double now = cui_time(ui);
    if (clicked) {
        if (g_last_clicked[idx] == idx && (now - g_last_click_time[idx]) < 0.35) {
            g_last_clicked[idx] = -1;
            return true;
        }
        g_last_clicked[idx] = idx;
        g_last_click_time[idx] = now;
    }
    return false;
}

static bool file_double_click(cui_ctx *ui, int idx, bool clicked) {
    double now = cui_time(ui);
    if (clicked) {
        if (g_file_last_clicked[idx] == idx && (now - g_file_last_click_time[idx]) < 0.35) {
            g_file_last_clicked[idx] = -1;
            return true;
        }
        g_file_last_clicked[idx] = idx;
        g_file_last_click_time[idx] = now;
    }
    return false;
}

/* ================================================================== */
/* 多实例 tab                                                          */
/* ================================================================== */

static void tab_open_shell_and_sftp(int k) {
    if (g_tab_sftp[k]) { sftp_free(g_tab_sftp[k]); g_tab_sftp[k] = NULL; }
    if (g_tab_chan[k]) {
        ssh_channel_close(g_tab_chan[k]); ssh_channel_free(g_tab_chan[k]); g_tab_chan[k] = NULL;
    }
    ssh_session s = g_tab_ssh[k];
    if (!s) return;

    g_tab_sftp[k] = sftp_new(s);
    if (g_tab_sftp[k]) {
        if (sftp_init(g_tab_sftp[k]) != SSH_OK) {
            log_status("SFTP init 失败：%s", ssh_get_error(s));
            sftp_free(g_tab_sftp[k]); g_tab_sftp[k] = NULL;
        } else log_status("SFTP 已建立");
    }

    ssh_channel ch = ssh_channel_new(s);
    if (ch && ssh_channel_open_session(ch) == SSH_OK &&
        ssh_channel_request_pty(ch) == SSH_OK &&
        ssh_channel_request_shell(ch) == SSH_OK) {
        g_tab_chan[k] = ch;
        g_tab_shell_ready[k] = true;
    } else if (ch) { ssh_channel_close(ch); ssh_channel_free(ch); }
}

static int open_tab_with_ssh(int sess_idx, ssh_session s) {
    if (sess_idx < 0 || sess_idx >= g_session_count) return -1;
    if (g_tab_count >= MAX_TABS) { if (s) { ssh_disconnect(s); ssh_free(s); } return -1; }
    int k = g_tab_count++;
    g_tabs[k].session_idx = sess_idx;
    g_tabs[k].inst_id = g_next_inst++;
    g_tabs[k].reconnect_attempts = 0;
    g_tabs[k].reconnect_at = 0;
    my_terminal_init(&g_tab_term[k], g_tab_term_text[k], sizeof g_tab_term_text[k],
                     g_tab_term_lines[k], (int)(sizeof g_tab_term_lines[k]/sizeof g_tab_term_lines[k][0]));
    g_tab_ssh[k] = s;
    g_tab_chan[k] = NULL;
    g_tab_sftp[k] = NULL;
    g_tab_shell_ready[k] = false;

    ssh_session_cfg *cfg = &g_sessions[sess_idx];
    char buf[256];
    snprintf(buf, sizeof buf, "Connected to %s@%s:%d\r\n", cfg->user, cfg->host, cfg->port);
    my_terminal_write(&g_tab_term[k], buf, -1);

    tab_open_shell_and_sftp(k);
    g_use_counter++;
    g_last_used[sess_idx] = g_use_counter;
    g_ssh_tab = k;
    /* ← 新增：自动启动会话里启用的隧道 */
    ssh_session_cfg *cfg_fwd = &g_sessions[sess_idx];
    for (int i = 0; i < cfg_fwd->fwd_count; i++) {
        if (cfg_fwd->fwd[i].active)
            start_forward(k, sess_idx, i);
    }
    return k;
}

static int open_tab_for_session(int sess_idx) {
    if (sess_idx < 0 || sess_idx >= g_session_count) return -1;
    int last = -1, last_use = -1;
    for (int k = 0; k < g_tab_count; k++)
        if (g_tabs[k].session_idx == sess_idx && g_last_used[sess_idx] > last_use) {
            last_use = g_last_used[sess_idx]; last = k;
        }
    if (last >= 0) { g_ssh_tab = last; return last; }
    ssh_session_cfg *s = &g_sessions[sess_idx];
    char pass[MAX_PASS] = "";
    decrypt_pass(s->pass_enc, pass);
    char err[256] = "";
    ssh_session conn = NULL;
    if (do_connect(&conn, s->host, s->port, s->user, pass, err, sizeof err)) {
        s->connected = true;
        return open_tab_with_ssh(sess_idx, conn);
    } else {
        s->auth_failed = true; g_retry_idx = sess_idx; g_retry_dlg = true;
        return -1;
    }
}

static int open_tab_for_session_new(int sess_idx) {
    if (sess_idx < 0 || sess_idx >= g_session_count) return -1;
    if (g_tab_count >= MAX_TABS) return -1;
    ssh_session_cfg *s = &g_sessions[sess_idx];
    char pass[MAX_PASS] = "";
    decrypt_pass(s->pass_enc, pass);
    char err[256] = "";
    ssh_session conn = NULL;
    if (do_connect(&conn, s->host, s->port, s->user, pass, err, sizeof err)) {
        s->connected = true;
        return open_tab_with_ssh(sess_idx, conn);
    } else {
        s->auth_failed = true; g_retry_idx = sess_idx; g_retry_dlg = true;
        return -1;
    }
}

/* ================================================================== */
/* 端口转发（本地转发 -L）                                             */
/* ================================================================== */

#ifdef _WIN32
#  define FW_CLOSE_SOCK closesocket
#  define FW_INVALID_SOCK INVALID_SOCKET
typedef SOCKET fw_socket_t;
#else
#  define FW_CLOSE_SOCK close
#  define FW_INVALID_SOCK (-1)
typedef int fw_socket_t;
#endif

static void forward_conn_close(forward_conn *c) {
    if (c->chan) {
        if (c->chan_open) {
            ssh_channel_send_eof(c->chan);
            ssh_channel_close(c->chan);
        }
        ssh_channel_free(c->chan);
        c->chan = NULL;
    }
    if (c->fd != (int)FW_INVALID_SOCK && c->fd >= 0) {
        FW_CLOSE_SOCK(c->fd);
        c->fd = -1;
    }
    c->chan_open = false;
}

static void stop_forward(int idx) {
    if (idx < 0 || idx >= g_forward_count) return;
    port_forward *pf = &g_forwards[idx];
    for (int k = 0; k < pf->conn_count; k++)
        forward_conn_close(&pf->conns[k]);
    pf->conn_count = 0;
    if (pf->listen_fd >= 0) {
        FW_CLOSE_SOCK(pf->listen_fd);
        pf->listen_fd = -1;
    }
    pf->running = false;
}

static void stop_forwards_for_tab(int tab_idx) {
    for (int i = 0; i < g_forward_count; i++)
        if (g_forwards[i].tab_idx == tab_idx && g_forwards[i].running)
            stop_forward(i);
}

static void start_forward(int tab_idx, int sess_idx, int cfg_idx) {
    if (tab_idx < 0 || tab_idx >= g_tab_count) return;
    if (sess_idx < 0 || sess_idx >= g_session_count) return;
    if (cfg_idx < 0 || cfg_idx >= g_sessions[sess_idx].fwd_count) return;

    /* 已有同 tab + cfg 的运行实例？先停 */
    for (int i = 0; i < g_forward_count; i++) {
        port_forward *old = &g_forwards[i];
        if (old->running && old->tab_idx == tab_idx &&
            old->sess_idx == sess_idx && old->cfg_idx == cfg_idx) {
            stop_forward(i);
        }
    }

    forward_cfg *cfg = &g_sessions[sess_idx].fwd[cfg_idx];
    if (!cfg->active) return;

    /* 找空槽 */
    // int slot = -1;
    // for (int i = 0; i < g_forward_count; i++) {
    //     if (!g_forwards[i].running && g_forwards[i].tab_idx == -1) { slot = i; break; }
    // }
    int slot = -1;
    for (int i = 0; i < g_forward_count; i++) {
        if (!g_forwards[i].running) { slot = i; break; }
    }
    if (slot < 0) {
        if (g_forward_count >= (int)(sizeof g_forwards / sizeof g_forwards[0])) return;
        slot = g_forward_count++;
    }
    port_forward *pf = &g_forwards[slot];
    memset(pf, 0, sizeof *pf);
    pf->tab_idx = tab_idx;
    pf->sess_idx = sess_idx;
    pf->cfg_idx = cfg_idx;
    pf->listen_fd = -1;

    /* 本地监听 socket */
#ifdef _WIN32
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        snprintf(pf->err, sizeof pf->err, "socket() failed: %d", WSAGetLastError());
        pf->tab_idx = -1;
        return;
    }
    fw_socket_t fd = s;
#else
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(pf->err, sizeof pf->err, "socket() failed: %d", errno);
        pf->tab_idx = -1;
        return;
    }
#endif

    /* 允许端口复用，避免 TIME_WAIT 占端口 */
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)cfg->local_port);
    if (!cfg->local_host[0] || !strcmp(cfg->local_host, "127.0.0.1") ||
        !strcmp(cfg->local_host, "localhost")) {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    } else if (!strcmp(cfg->local_host, "0.0.0.0")) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
#ifdef _WIN32
        if (InetPtonA(AF_INET, cfg->local_host, &addr.sin_addr) != 1) {
#else
        if (inet_pton(AF_INET, cfg->local_host, &addr.sin_addr) != 1) {
#endif
            snprintf(pf->err, sizeof pf->err, "invalid local host: %s", cfg->local_host);
            FW_CLOSE_SOCK(fd);
            pf->tab_idx = -1;
            return;
        }
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
#ifdef _WIN32
        snprintf(pf->err, sizeof pf->err, "bind(%s:%d) failed: %d", cfg->local_host, cfg->local_port, WSAGetLastError());
#else
        snprintf(pf->err, sizeof pf->err, "bind(%s:%d) failed: %d", cfg->local_host, cfg->local_port, errno);
#endif
        FW_CLOSE_SOCK(fd);
        pf->tab_idx = -1;
        return;
    }
    if (listen(fd, 4) != 0) {
        snprintf(pf->err, sizeof pf->err, "listen() failed");
        FW_CLOSE_SOCK(fd);
        pf->tab_idx = -1;
        return;
    }

    /* 非阻塞 */
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif

    pf->listen_fd = (int)fd;
    pf->running = true;
    pf->err[0] = 0;
    log_status(T("端口转发已启动：%s:%d → %s:%d",
                 "Forward started: %s:%d → %s:%d"),
               cfg->local_host, cfg->local_port, cfg->remote_host, cfg->remote_port);
}

static void forward_accept(port_forward *pf) {
    struct sockaddr_in peer;
    socklen_t plen = sizeof peer;
    fw_socket_t cfd = accept(pf->listen_fd, (struct sockaddr *)&peer, &plen);
    if (cfd == FW_INVALID_SOCK) return;

    if (pf->conn_count >= MAX_FWD_CONNS) {
        FW_CLOSE_SOCK(cfd);
        return;
    }

    /* 非阻塞 */
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(cfd, FIONBIO, &nb);
#else
    int flags = fcntl(cfd, F_GETFL, 0);
    fcntl(cfd, F_SETFL, flags | O_NONBLOCK);
#endif

    forward_cfg *cfg = &g_sessions[pf->sess_idx].fwd[pf->cfg_idx];
    ssh_session ss = g_tab_ssh[pf->tab_idx];
    if (!ss) { FW_CLOSE_SOCK(cfd); return; }

    ssh_channel ch = ssh_channel_new(ss);
    if (!ch) { FW_CLOSE_SOCK(cfd); return; }

    int rc = ssh_channel_open_forward(ch, cfg->remote_host, cfg->remote_port,
                                      cfg->local_host, cfg->local_port);
    if (rc != SSH_OK) {
        snprintf(pf->err, sizeof pf->err, "open_forward failed: %s", ssh_get_error(ss));
        ssh_channel_free(ch);
        FW_CLOSE_SOCK(cfd);
        return;
    }

    forward_conn *c = &pf->conns[pf->conn_count++];
    c->fd = (int)cfd;
    c->chan = ch;
    c->chan_open = true;
    c->bytes_in = c->bytes_out = 0;
}

static void forward_pump(port_forward *pf, forward_conn *c) {
    char buf[16384];

    /* 本地 → 远端 */
    int n = recv(c->fd, buf, sizeof buf, 0);
    if (n > 0) {
        if (ssh_channel_write(c->chan, buf, (uint32_t)n) != n) {
            forward_conn_close(c);
            return;
        }
        c->bytes_out += (uint64_t)n;
    } else if (n == 0) {
        forward_conn_close(c);
        return;
    } else {
#ifdef _WIN32
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK && e != WSAEINTR) { forward_conn_close(c); return; }
#else
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            forward_conn_close(c);
            return;
        }
#endif
    }

    /* 远端 → 本地 */
    while (1) {
        int r = ssh_channel_read_nonblocking(c->chan, buf, sizeof buf, 0);
        if (r > 0) {
            int sent = 0;
            while (sent < r) {
                int w = send(c->fd, buf + sent, r - sent, 0);
                if (w <= 0) {
#ifdef _WIN32
                    int e = WSAGetLastError();
                    if (e == WSAEWOULDBLOCK) { Sleep(1); continue; }
#else
                    if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(1000); continue; }
#endif
                    forward_conn_close(c);
                    return;
                }
                sent += w;
            }
            c->bytes_in += (uint64_t)r;
        } else {
            break;
        }
    }
    if (ssh_channel_is_eof(c->chan) || ssh_channel_is_closed(c->chan))
        forward_conn_close(c);
}

static void poll_forwards(cui_ctx *ui) {
    (void)ui;
    for (int i = 0; i < g_forward_count; i++) {
        port_forward *pf = &g_forwards[i];
        if (!pf->running) continue;

        /* 接受新连接 */
        if (pf->listen_fd >= 0) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(pf->listen_fd, &rfds);
            struct timeval tv = { 0, 0 };
            if (select(pf->listen_fd + 1, &rfds, NULL, NULL, &tv) > 0 &&
                FD_ISSET(pf->listen_fd, &rfds)) {
                forward_accept(pf);
            }
        }

        /* pump 所有连接 */
        for (int k = 0; k < pf->conn_count; k++) {
            forward_conn *c = &pf->conns[k];
            if (c->fd < 0) continue;
            forward_pump(pf, c);
        }

        /* 清理已关闭的连接 */
        int w = 0;
        for (int k = 0; k < pf->conn_count; k++) {
            if (pf->conns[k].fd >= 0) pf->conns[w++] = pf->conns[k];
        }
        pf->conn_count = w;
    }
}

static void close_tab(int tab_idx) {
    if (tab_idx < 0 || tab_idx >= g_tab_count) return;
    stop_forwards_for_tab(tab_idx);
    if (g_tab_sftp[tab_idx]) { sftp_free(g_tab_sftp[tab_idx]); g_tab_sftp[tab_idx] = NULL; }
    if (g_tab_chan[tab_idx]) {
        ssh_channel_send_eof(g_tab_chan[tab_idx]);
        ssh_channel_close(g_tab_chan[tab_idx]);
        ssh_channel_free(g_tab_chan[tab_idx]);
        g_tab_chan[tab_idx] = NULL;
    }
    g_tab_shell_ready[tab_idx] = false;
    if (g_tab_ssh[tab_idx]) {
        ssh_disconnect(g_tab_ssh[tab_idx]);
        ssh_free(g_tab_ssh[tab_idx]);
        g_tab_ssh[tab_idx] = NULL;
    }
    for (int k = tab_idx; k < g_tab_count - 1; k++) {
        g_tabs[k] = g_tabs[k + 1];
        g_tab_ssh[k] = g_tab_ssh[k + 1];
        g_tab_chan[k] = g_tab_chan[k + 1];
        g_tab_sftp[k] = g_tab_sftp[k + 1];
        g_tab_shell_ready[k] = g_tab_shell_ready[k + 1];
        memcpy(&g_tab_term[k], &g_tab_term[k + 1], sizeof g_tab_term[k]);
        memcpy(g_tab_term_text[k], g_tab_term_text[k + 1], sizeof g_tab_term_text[k]);
        memcpy(g_tab_term_lines[k], g_tab_term_lines[k + 1], sizeof g_tab_term_lines[k]);
        g_tab_term[k].text = g_tab_term_text[k];       /* ← 新增：重设 buffer 指针 */
        g_tab_term[k].lines = g_tab_term_lines[k];     /* ← 新增 */
    }
    g_tab_count--;
    if (g_ssh_tab >= g_tab_count) g_ssh_tab = g_tab_count > 0 ? g_tab_count - 1 : 0;
    g_last_fm_tab = -1;          /* ← 新增：强制文件表下帧重置 */
}

/* 断线重连检测 */
static void check_reconnects(cui_ctx *ui) {
    double now = cui_time(ui);
    for (int k = 0; k < g_tab_count; k++) {
        if (!g_tab_ssh[k]) continue;
        if (ssh_is_connected(g_tab_ssh[k])) { g_tabs[k].reconnect_attempts = 0; continue; }
        if (g_tabs[k].reconnect_at > now) continue;
        /* 重连 */
        ssh_session_cfg *cfg = &g_sessions[g_tabs[k].session_idx];
        char pass[MAX_PASS] = "";
        decrypt_pass(cfg->pass_enc, pass);
        char err[256] = "";
        ssh_session new_s = NULL;
        log_status(T("重连 tab %d（尝试 %d）...", "Reconnecting tab %d (attempt %d)..."), k, g_tabs[k].reconnect_attempts + 1);
        if (do_connect(&new_s, cfg->host, cfg->port, cfg->user, pass, err, sizeof err)) {
            ssh_disconnect(g_tab_ssh[k]);
            ssh_free(g_tab_ssh[k]);
            g_tab_ssh[k] = new_s;
            g_tabs[k].reconnect_attempts = 0;
            tab_open_shell_and_sftp(k);
            my_terminal_write(&g_tab_term[k], "\r\n[reconnected]\r\n", -1);
        } else {
            g_tabs[k].reconnect_attempts++;
            double delay = 2.0;
            for (int i = 0; i < g_tabs[k].reconnect_attempts && i < 5; i++) delay *= 2;
            if (delay > 60.0) delay = 60.0;
            g_tabs[k].reconnect_at = now + delay;
            log_status_toast(ui, CUI_TONE_WARNING, T("重连失败，%0.f 秒后重试：%s", "Reconnect failed, retrying in %0.f s: %s"), delay, err);
        }
    }
}

/* ================================================================== */
/* 左栏上：会话列表                                                    */
/* ================================================================== */

static void sessions_panel(cui_ctx *ui) {
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 4) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .width = CUI_GROW(1)) {
                cui_text(ui, T("会话", "Sessions"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
            }
            if (cui_icon_button(ui, CUI_ICON_PLUS, .tooltip = T("新建会话", "New session"), .size = CUI_SIZE_SM)) {
                g_new_dlg = true;
                dlg_name[0] = dlg_host[0] = dlg_user[0] = dlg_pass[0] = 0;
                dlg_port = 22; g_ctx_target = -1;
            }
        }
        hline(ui);
        cui_scroll(ui, .id = "sess-rows", .height = CUI_GROW(1)) {
            for (int i = 0; i < g_session_count; i++) {
                ssh_session_cfg *s = &g_sessions[i];
                cui_push_id_int(ui, i);
                bool is_sel = g_selected == i;
                char label[160];
                snprintf(label, sizeof label, "%s  %s", s->name, s->user);
                bool clicked = cui_button(ui, label, .variant = CUI_BUTTON_GHOST,
                                          .full_width = true, .size = CUI_SIZE_SM, .selected = is_sel);
                if (clicked) {
                    g_selected = i;
                    if (is_double_click(ui, i, true)) {
                        log_status(T("双击会话 %s，尝试连接 %s:%d",
                                     "Double-click session %s, connecting %s:%d"),
                                   s->name, s->host, s->port);
                        int t = open_tab_for_session(i);
                        if (t < 0)
                            log_status_toast(ui, CUI_TONE_DANGER,
                                             T("连接 %s 失败", "Failed to connect to %s"), s->name);
                        else
                            log_status_toast(ui, CUI_TONE_SUCCESS,
                                             T("已连接 %s", "Connected to %s"), s->name);
                    } else {
                        for (int k = 0; k < g_tab_count; k++)
                            if (g_tabs[k].session_idx == i) { g_ssh_tab = k; break; }
                    }
                }
                char ctx_id[64];
                snprintf(ctx_id, sizeof ctx_id, "sess-ctx-%d", i);
                cui_context_menu(ui, ctx_id) {
                    if (cui_menu_item(ui, T("打开", "Open"), .icon = CUI_ICON_EXTERNAL_LINK))
                        open_tab_for_session_new(i);
                    if (cui_menu_item(ui, T("新建", "New"), .icon = CUI_ICON_PLUS)) {
                        g_new_dlg = true;
                        dlg_name[0] = dlg_host[0] = dlg_user[0] = dlg_pass[0] = 0;
                        dlg_port = 22; g_ctx_target = -1;
                    }
                    if (cui_menu_item(ui, T("编辑", "Edit"), .icon = CUI_ICON_PENCIL)) {
                        g_ctx_target = i;
                        ssh_session_cfg *e = &g_sessions[i];
                        snprintf(dlg_name, sizeof dlg_name, "%s", e->name);
                        snprintf(dlg_host, sizeof dlg_host, "%s", e->host);
                        dlg_port = e->port;
                        snprintf(dlg_user, sizeof dlg_user, "%s", e->user);
                        decrypt_pass(e->pass_enc, dlg_pass);
                        g_edit_dlg = true;
                    }
                    if (cui_menu_item(ui, T("删除", "Delete"), .icon = CUI_ICON_TRASH_2)) {
                        g_ctx_target = i; g_del_dlg = true;
                    }
                }
                cui_pop_id(ui);
            }
            if (g_session_count == 0)
                cui_text(ui, T("还没有会话，点右上角 + 新建", "No sessions yet — use + above to add one"),
                         .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        }
    }
}

/* ================================================================== */
/* 左栏下：统计                                                        */
/* ================================================================== */

static void progress_with_text(cui_ctx *ui, const char *label, float pct,
                               unsigned long long used, unsigned long long total) {
    char line[160];
    if (total > 0)
        snprintf(line, sizeof line, "%s  %.1f%%  (%.1f / %.1f GB)", label, pct, used/1073741824.0, total/1073741824.0);
    else
        snprintf(line, sizeof line, "%s  %.1f%%", label, pct);
    cui_text(ui, line, .size = 12);
    cui_progress(ui, pct / 100.0f, .height = 6);
}

static void stats_panel(cui_ctx *ui) {
    cui_column(ui, .width = CUI_FILL, .gap = 8) {
        cui_row(ui, .width = CUI_FILL, .gap = 8) {
            cui_text(ui, T("IP", "IP"), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
            /* IP 值 + 复制按钮，按钮手画成与文本同高 */
            const char *ip_str = g_selected >= 0
                                 ? g_sessions[g_selected].host
                                 : "192.168.1.10";
            cui_text(ui, ip_str, .font = CUI_FONT_MONO, .nowrap = true);

            /* 复制按钮：16x16，与 11 号字视觉对齐 */
            cui_id copy_id = cui_make_id(ui, "stats-ip-copy");
            int copy_node = cui__node_new(ui, copy_id);
            ui->nodes[copy_node].w = CUI_PX(16);
            ui->nodes[copy_node].h = CUI_PX(16);
            ui->nodes[copy_node].radius = CUI_CORNERS(3);
            ui->nodes[copy_node].bg = CUI_TRANSPARENT;
            cui__open(ui, copy_node);
                cui_icon(ui, CUI_ICON_COPY, 12, CUI_COLOR_TEXT_SECONDARY);
            cui__close(ui);
            cui__mark_interactive(ui, copy_node, 0);

            cui_interact cvr = cui__interact(ui, copy_id, 0);
            static bool prev_pressed_ip = false;
            bool clicked_now = cvr.pressed && !prev_pressed_ip;
            prev_pressed_ip = cvr.pressed;
            if (clicked_now) {
                if (ui->platform.set_clipboard) {
                    ui->platform.set_clipboard(ui->platform.user, ip_str);
                    log_status(T("已复制 IP：%s", "IP copied: %s"), ip_str);
                    char title[256];
                    snprintf(title, sizeof title, T("已复制 IP", "IP copied"));
                    cui_toast(ui, title,
                              .description = ip_str,
                              .tone = CUI_TONE_SUCCESS,
                              .duration = 2.0f);
                } else {
                    log_status(T("剪贴板不可用，无法复制 IP", "Clipboard unavailable"));
                    char title[256];
                    snprintf(title, sizeof title,
                             T("剪贴板不可用", "Clipboard unavailable"));
                    cui_toast(ui, title,
                              .description = ip_str,
                              .tone = CUI_TONE_DANGER,
                              .duration = 2.0f);
                }
            }
            if (cvr.hovered) {
                ui->nodes[copy_node].bg = cui_alpha(CUI_C(ui, ACCENT), 0.15f);
                cui__set_cursor(ui, CUI_CURSOR_HAND);
            }
        }
        cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, T("运行", "Uptime"), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
            char up_buf[32];
            unsigned long long days = g_uptime_sec / 86400;
            unsigned long long hours = (g_uptime_sec % 86400) / 3600;
            if (days > 0) snprintf(up_buf, sizeof up_buf, "%llud %lluh", days, hours);
            else          snprintf(up_buf, sizeof up_buf, "%lluh %llum", hours, (g_uptime_sec % 3600) / 60);
            cui_text(ui, up_buf, .font = CUI_FONT_MONO, .nowrap = true);
        }
        cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, T("负载", "Load"), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
            char buf[64];
            snprintf(buf, sizeof buf, "%.2f  %.2f  %.2f", g_load[0], g_load[1], g_load[2]);
            cui_text(ui, buf, .font = CUI_FONT_MONO, .nowrap = true);
        }
        progress_with_text(ui, "CPU", g_cpu, 0, 0);
        progress_with_text(ui, T("内存", "RAM"), g_ram, g_ram_used, g_ram_total);
        progress_with_text(ui, T("交换", "Swap"), g_swap, g_swap_used, g_swap_total);

        hline(ui);
        cui_text(ui, T("进程（按内存）", "Top processes"), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
        cui_row(ui, .width = CUI_FILL, .gap = 4) {
            sortable_header(ui, "proc", T("命令", "Command"), 0, true, CUI_TEXT_LEFT, &g_proc_sort, 0);
            sortable_header(ui, "proc", "CPU%", 70, false, CUI_TEXT_CENTER, &g_proc_sort, 1);
            sortable_header(ui, "proc", "MEM%", 70, false, CUI_TEXT_CENTER, &g_proc_sort, 2);
        }
        hline(ui);
        int pidx[PROC_COUNT];
        sort_indices(pidx, PROC_COUNT, g_proc_sort, g_procs, sizeof g_procs[0], proc_cmp);
        cui_scroll(ui, .id = "proc-scroll", .height = CUI_PX(96)) {
            for (int k = 0; k < PROC_COUNT; k++) {
                int i = pidx[k];
                if (!g_procs[i].cmd || !g_procs[i].cmd[0]) continue;
                cui_row(ui, .width = CUI_FILL, .gap = 4, .pad_y = 3) {
                    cui_box(ui, .width = CUI_GROW(1)) { cui_text(ui, g_procs[i].cmd, .nowrap = true, .size = 12); }
                    cui_box(ui, .width = CUI_PX(56)) { cui_textf(ui, "%.1f", g_procs[i].cpu); }
                    cui_box(ui, .width = CUI_PX(56)) { cui_textf(ui, "%.1f", g_procs[i].mem); }
                }
            }
        }

        hline(ui);
        float rx_vals[NET_HIST], tx_vals[NET_HIST];
        for (int k = 0; k < NET_HIST; k++) {
            int idx = (g_net_head + k) % NET_HIST;
            rx_vals[k] = g_rx_hist[idx]; tx_vals[k] = g_tx_hist[idx];
        }
        cui_series net_series[2] = {
            { "rx", rx_vals, NET_HIST, CUI_RGBA(0x3E7BF9FF) },
            { "tx", tx_vals, NET_HIST, CUI_RGBA(0xFF8F8FFF) },
        };
        cui_bar_chart(ui, "net-chart", net_series, 2, .height = 80, .no_legend = true, .no_grid = true);
    }
}

static void dirs_panel(cui_ctx *ui) {
    cui_column(ui, .width = CUI_FILL, .height = CUI_FIT, .gap = 4) {
        cui_text(ui, T("目录用量", "Directory usage"), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
        cui_row(ui, .width = CUI_FILL, .gap = 4) {
            sortable_header(ui, "dirs", T("路径", "Path"), 0, true, CUI_TEXT_LEFT, &g_dir_sort, 0);
            sortable_header(ui, "dirs", T("已用/总量", "Used / Total"), 120, false, CUI_TEXT_CENTER, &g_dir_sort, 1);
        }
        hline(ui);
        int didx[64];
        int dn = g_dir_count < 64 ? g_dir_count : 64;
        sort_indices(didx, dn, g_dir_sort, g_dirs, sizeof g_dirs[0], dir_cmp);
        cui_scroll(ui, .id = "dirs-scroll", .height = CUI_PX(140)) {
            for (int k = 0; k < dn; k++) {
                int i = didx[k];
                cui_row(ui, .width = CUI_FILL, .gap = 4, .pad_y = 3) {
                    cui_box(ui, .width = CUI_GROW(1)) { cui_text(ui, g_dirs[i].path, .nowrap = true, .size = 12); }
                    cui_box(ui, .width = CUI_PX(120)) {
                        char buf[64];
                        snprintf(buf, sizeof buf, "%s / %s", g_dirs[i].used, g_dirs[i].total);
                        cui_text(ui, buf, .nowrap = true, .size = 12);
                    }
                }
            }
        }
    }
}

/* ================================================================== */
/* 右栏上：SSH tabs                                                    */
/* ================================================================== */

static void poll_tab_shell(cui_ctx *ui, int k) {
    (void)ui;
    if (k < 0 || k >= g_tab_count) return;
    ssh_channel ch = g_tab_chan[k];
    if (!ch) return;
    char buf[8192]; int total = 0;
    while (total < (int)sizeof buf - 1) {
        int r = ssh_channel_read_nonblocking(ch, buf + total, (uint32_t)(sizeof buf - 1 - total), 0);
        if (r <= 0) break;
        total += r;
        if (r < 1024) break;
    }
    if (total > 0) {
        buf[total] = 0;
        my_terminal_write(&g_tab_term[k], buf, total);
    }
    if (ssh_channel_is_eof(ch) || ssh_channel_is_closed(ch)) {
        my_terminal_write(&g_tab_term[k], "\r\n[shell closed]\r\n", -1);
        g_tab_shell_ready[k] = false;
        ssh_channel_close(ch); ssh_channel_free(ch); g_tab_chan[k] = NULL;
    }
}

static void terminals_panel1(cui_ctx *ui) {
    if (g_tab_count == 0) {
        cui_text(ui, T("双击左侧会话打开终端", "Double-click a session to open"),
                 .color = CUI_COLOR_TEXT_SECONDARY);
        return;
    }
    if (g_ssh_tab < 0 || g_ssh_tab >= g_tab_count) g_ssh_tab = 0;
    // if (g_tab_chan[g_ssh_tab]) {
    //     poll_tab_shell(ui, g_ssh_tab);
    //     cui_request_wakeup(ui, 0.05f);
    // }
    /* 轮询所有 tab */
    for (int k = 0; k < g_tab_count; k++) {
        if (g_tab_chan[k]) poll_tab_shell(ui, k);
    }
    if (g_tab_count > 0) cui_request_wakeup(ui, 0.05f);
    
    static char titles[MAX_TABS][96];
    cui_doc_tab tabs[MAX_TABS];
    for (int k = 0; k < g_tab_count; k++) {
        const char *name = g_sessions[g_tabs[k].session_idx].name;
        int dup = 0;
        for (int m = 0; m < k; m++)
            if (g_tabs[m].session_idx == g_tabs[k].session_idx) dup++;
        if (dup > 0) snprintf(titles[k], sizeof titles[k], "%s (%d)", name, dup + 1);
        else snprintf(titles[k], sizeof titles[k], "%s", name);
        tabs[k].title = titles[k]; tabs[k].icon = NULL; tabs[k].tooltip = NULL; tabs[k].dirty = false;
    }
    static cui_doc_tabs_opts dtopts = { 0 };
    cui_doc_tabs_result r = cui_doc_tabs_ex(ui, "ssh-tabs", tabs, g_tab_count, &g_ssh_tab, &dtopts);
    if (r.close >= 0 && r.close < g_tab_count) { close_tab(r.close); return; }
    /* ← 新增：端口转发按钮 */
    // if (cui_icon_button(ui, CUI_ICON_ACTIVITY,
    //                     .tooltip = T("端口转发", "Port forwarding"),
    //                     .size = CUI_SIZE_SM)) {
    //     g_forwards_dlg = true;
    // }
    my_terminal_opts topts = { ._ = 0, .height = -1.0f, .prompt = "$ ", .size=g_term_font_size };
    my_terminal_result res = my_terminal_ex(ui, "term-active", &g_tab_term[g_ssh_tab], &topts);
    if (g_tab_chan[g_ssh_tab]) {
        if (res.interrupt) {
            ssh_channel_write(g_tab_chan[g_ssh_tab], "\x03", 1);
            cui_request_wakeup(ui, 0.05f);
        } else if (res.raw_input && res.raw_input[0]) {
            ssh_channel_write(g_tab_chan[g_ssh_tab], res.raw_input, (uint32_t)strlen(res.raw_input));
            cui_request_wakeup(ui, 0.05f);
        }
    }
}

static void terminals_panel(cui_ctx *ui) {
    if (g_tab_count == 0) {
        cui_text(ui, T("双击左侧会话打开终端", "Double-click a session to open"),
                 .color = CUI_COLOR_TEXT_SECONDARY);
        return;
    }
    if (g_ssh_tab < 0 || g_ssh_tab >= g_tab_count) g_ssh_tab = 0;
    /* 轮询所有 tab */
    for (int k = 0; k < g_tab_count; k++) {
        if (g_tab_chan[k]) poll_tab_shell(ui, k);
    }
    if (g_tab_count > 0) cui_request_wakeup(ui, 0.05f);

    static char titles[MAX_TABS][96];
    cui_doc_tab tabs[MAX_TABS];
    for (int k = 0; k < g_tab_count; k++) {
        const char *name = g_sessions[g_tabs[k].session_idx].name;
        int dup = 0;
        for (int m = 0; m < k; m++)
            if (g_tabs[m].session_idx == g_tabs[k].session_idx) dup++;
        if (dup > 0) snprintf(titles[k], sizeof titles[k], "%s (%d)", name, dup + 1);
        else snprintf(titles[k], sizeof titles[k], "%s", name);
        tabs[k].title = titles[k]; tabs[k].icon = NULL; tabs[k].tooltip = NULL; tabs[k].dirty = false;
    }
    static cui_doc_tabs_opts dtopts = { 0 };

    /* tab 栏 + 右侧端口转发按钮，同一行 */
    cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 4) {
        cui_box(ui, .width = CUI_GROW(1)) {
            cui_doc_tabs_result r = cui_doc_tabs_ex(ui, "ssh-tabs", tabs, g_tab_count, &g_ssh_tab, &dtopts);
            if (r.close >= 0 && r.close < g_tab_count) { close_tab(r.close); return; }
        }
        if (cui_icon_button(ui, CUI_ICON_ACTIVITY,
                            .tooltip = T("端口转发", "Port forwarding"),
                            .size = CUI_SIZE_SM)) {
            g_forwards_dlg = true;
        }
    }

        my_terminal_opts topts = { ._ = 0, .height = -1.0f, .prompt = "$ ",
                               .size = g_term_font_size, .lang = lang };
    my_terminal_result res = my_terminal_ex(ui, "term-active", &g_tab_term[g_ssh_tab], &topts);
    if (g_tab_chan[g_ssh_tab]) {
        if (res.interrupt) {
            ssh_channel_write(g_tab_chan[g_ssh_tab], "\x03", 1);
            cui_request_wakeup(ui, 0.05f);
        } else if (res.raw_input && res.raw_input[0]) {
            ssh_channel_write(g_tab_chan[g_ssh_tab], res.raw_input, (uint32_t)strlen(res.raw_input));
            cui_request_wakeup(ui, 0.05f);
        }
        if (res.paste) {
            const char *clip = NULL;
            if (ui->platform.get_clipboard)
                clip = ui->platform.get_clipboard(ui->platform.user);
            if (clip && clip[0]) {
                char buf[4096]; size_t o = 0;
                for (const char *p = clip; *p && o + 1 < sizeof buf; p++) {
                    if (*p == '\n') buf[o++] = '\r';
                    else if (*p != '\r') buf[o++] = *p;
                }
                buf[o] = 0;
                if (o > 0) ssh_channel_write(g_tab_chan[g_ssh_tab], buf, (uint32_t)o);
            }
        }
        
    }
    if (res.clear) {
        my_terminal_clear(&g_tab_term[g_ssh_tab]);
    }
}

static bool g_nav_error_dlg = false;
static char g_nav_error_path[512] = "";
/* 规范化路径：去掉末尾 /，合并 //，确保 / 开头 */
static void normalize_path(const char *in, char *out, size_t cap) {
    if (!in || !*in) { snprintf(out, cap, "/"); return; }
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", in);
    /* 合并 // */
    char *p = tmp;
    while ((p = strstr(p, "//")) != NULL) {
        memmove(p, p + 1, strlen(p + 1) + 1);
        p++;
    }
    /* 去末尾 / */
    size_t n = strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
    if (tmp[0] != '/') {
        snprintf(out, cap, "/%s", tmp);
    } else {
        snprintf(out, cap, "%s", tmp);
    }
}

/* 路径存在（能 sftp_opendir） */
static bool path_exists_remote(const char *path) {
    if (g_ssh_tab < 0 || g_ssh_tab >= g_tab_count) return false;
    sftp_session sftp = g_tab_sftp[g_ssh_tab];
    if (!sftp) return false;
    sftp_dir d = sftp_opendir(sftp, path);
    if (!d) return false;
    sftp_closedir(d);
    return true;
}

/* 导航到路径：验证 → 扫描 → 展开树 */
static bool navigate_to(const char *raw_path) {
    char path[512];
    normalize_path(raw_path, path, sizeof path);

    if (!path_exists_remote(path)) {
        /* 弹框提示 */
        g_nav_error_dlg = true;
        snprintf(g_nav_error_path, sizeof g_nav_error_path, "%s", path);
        return false;
    }

    scan_remote(path);
    tree_ensure_path(path);
    tree_load_children(path);

    /* 沿路径链逐级加载父节点，确保树展开 */
    char cur[512] = "";
    const char *p = path;
    while (*p) {
        if (*p == '/') p++;
        if (!*p) break;
        const char *end = strchr(p, '/');
        int len = end ? (int)(end - p) : (int)strlen(p);
        char seg[256];
        if (len >= (int)sizeof seg) len = sizeof seg - 1;
        memcpy(seg, p, len); seg[len] = 0;
        strcat(cur, "/"); strcat(cur, seg);
        int ai = tree_find(cur);
        if (ai >= 0) {
            g_tree[ai].open = true;
            tree_load_children(cur);
        }
        p += len;
    }
    return true;
}


static void nav_error_dialog(cui_ctx *ui) {
    if (!g_nav_error_dlg) return;
    cui_dialog(ui, T("路径不存在", "Path not found"), &g_nav_error_dlg, .width = 380) {
        cui_textf(ui, T("路径不存在：%s", "Path does not exist: %s"), g_nav_error_path);
        cui_dialog_footer(ui) {
            if (cui_button(ui, T("确定", "OK"), .variant = CUI_BUTTON_PRIMARY)) {
                g_nav_error_dlg = false;
            }
        }
    }
}


#ifdef _WIN32
static void upload_dir_collect(const char *local_dir, const char *remote_dir, uint64_t *total_bytes) {
    sftp_session sftp = g_tab_sftp[g_ssh_tab];
    if (sftp) sftp_mkdir(sftp, remote_dir, 0755);

    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s\\*", local_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char local_sub[1024];
        snprintf(local_sub, sizeof local_sub, "%s\\%s", local_dir, fd.cFileName);
        char remote_sub[1024];
        snprintf(remote_sub, sizeof remote_sub, "%s/%s", remote_dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            upload_dir_collect(local_sub, remote_sub, total_bytes);
        } else {
            if (g_upload_dir_file_count_total < MAX_DIR_QUEUE) {
                int k = g_upload_dir_file_count_total++;
                snprintf(g_upload_dir_files[k], sizeof g_upload_dir_files[k], "%s", local_sub);
                snprintf(g_upload_dir_remotes[k], sizeof g_upload_dir_remotes[k], "%s", remote_sub);
                *total_bytes += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void upload_dir_recursive(cui_ctx *ui, int tab_idx, const char *local_dir, const char *remote_dir) {
    g_upload_dir_file_count_total = 0;
    g_upload_dir_file_count_done = 0;
    g_upload_dir_next_idx = 0;
    g_upload_dir_active = true;
    const char *base = strrchr(local_dir, '\\');
    if (!base) base = strrchr(local_dir, '/');
    base = base ? base + 1 : local_dir;
    snprintf(g_upload_dir_name, sizeof g_upload_dir_name, "%s", base);
    snprintf(g_upload_dir_remote, sizeof g_upload_dir_remote, "%s", remote_dir);

    /* 收集文件列表 */
    uint64_t total_bytes = 0;
    upload_dir_collect(local_dir, remote_dir, &total_bytes);

    /* 一次性检查远程空间 */
    if (!check_remote_space_for_upload(ui, tab_idx, remote_dir, total_bytes)) {
        g_upload_dir_active = false;
        g_upload_dir_file_count_total = 0;
        return;
    }

    char title[256];
    snprintf(title, sizeof title, T("正在上传文件夹：%s（%d 个文件）",
                                    "Uploading folder: %s (%d files)"),
             g_upload_dir_name, g_upload_dir_file_count_total);
    cui_toast(ui, title,
              .description = g_upload_dir_remote,
              .tone = CUI_TONE_INFO,
              .duration = 2.0f);
}

/* 拖放展开：把一个路径（文件或目录）递归展开到 g_upload_dir_* 队列。
 * remote_dir 是该路径在远端的落点目录（不含文件名本身）。 */
static void drop_collect_path(cui_ctx *ui, const char *local_path, const char *remote_dir,
                              uint64_t *total_bytes) {
    if (g_upload_dir_file_count_total >= MAX_DIR_QUEUE) {
        log_status_toast(ui, CUI_TONE_WARNING,
                         T("文件过多，仅上传前 %d 个", "Too many files, uploading first %d"),
                         MAX_DIR_QUEUE);
    }
    /* 目录：递归 */
    DWORD attr = GetFileAttributesA(local_path);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        const char *base = strrchr(local_path, '\\');
        if (!base) base = strrchr(local_path, '/');
        base = base ? base + 1 : local_path;

        char remote_sub[1024];
        snprintf(remote_sub, sizeof remote_sub, "%s/%s", remote_dir, base);
        sftp_session sftp = g_tab_sftp[g_ssh_tab];
        if (sftp) sftp_mkdir(sftp, remote_sub, 0755);

        char pattern[1024];
        snprintf(pattern, sizeof pattern, "%s\\*", local_path);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
                char local_sub[1024];
                snprintf(local_sub, sizeof local_sub, "%s\\%s", local_path, fd.cFileName);
                drop_collect_path(ui, local_sub, remote_sub, total_bytes);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        return;
    }

    /* 普通文件：加入队列 */
    const char *base = strrchr(local_path, '\\');
    if (!base) base = strrchr(local_path, '/');
    base = base ? base + 1 : local_path;

    if (g_upload_dir_file_count_total >= MAX_DIR_QUEUE) return;
    int k = g_upload_dir_file_count_total++;
    snprintf(g_upload_dir_files[k], sizeof g_upload_dir_files[k], "%s", local_path);
    snprintf(g_upload_dir_remotes[k], sizeof g_upload_dir_remotes[k], "%s/%s", remote_dir, base);

    /* 累加字节数 */
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(local_path, GetFileExInfoStandard, &fad)) {
        *total_bytes += ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    }
}
#else
static void drop_collect_path(cui_ctx *ui, const char *local_path, const char *remote_dir,
                              uint64_t *total_bytes) {
    struct stat st;
    if (stat(local_path, &st) != 0) return;
    
    if (g_upload_dir_file_count_total >= MAX_DIR_QUEUE) {
        log_status_toast(ui, CUI_TONE_WARNING,
                         T("文件过多，仅上传前 %d 个", "Too many files, uploading first %d"),
                         MAX_DIR_QUEUE);
    }
    
    const char *base = strrchr(local_path, '/');
    base = base ? base + 1 : local_path;

    if (S_ISDIR(st.st_mode)) {
        char remote_sub[1024];
        snprintf(remote_sub, sizeof remote_sub, "%s/%s", remote_dir, base);
        sftp_session sftp = g_tab_sftp[g_ssh_tab];
        if (sftp) sftp_mkdir(sftp, remote_sub, 0755);

        DIR *d = opendir(local_path);
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char local_sub[1024];
            snprintf(local_sub, sizeof local_sub, "%s/%s", local_path, e->d_name);
            drop_collect_path(ui, local_sub, remote_sub, total_bytes);
        }
        closedir(d);
        return;
    }

    if (g_upload_dir_file_count_total >= MAX_DIR_QUEUE) return;
    int k = g_upload_dir_file_count_total++;
    snprintf(g_upload_dir_files[k], sizeof g_upload_dir_files[k], "%s", local_path);
    snprintf(g_upload_dir_remotes[k], sizeof g_upload_dir_remotes[k], "%s/%s", remote_dir, base);
    *total_bytes += (uint64_t)st.st_size;
}
#endif

static bool delete_dir_recursive(int tab_idx, const char *path) {
    if (tab_idx < 0 || tab_idx >= g_tab_count) return false;
    sftp_session sftp = g_tab_sftp[tab_idx];
    if (!sftp) return false;

    sftp_dir dir = sftp_opendir(sftp, path);
    if (!dir) return false;

    sftp_attributes attr;
    while ((attr = sftp_readdir(sftp, dir)) != NULL) {
        if (!strcmp(attr->name, ".") || !strcmp(attr->name, "..")) {
            sftp_attributes_free(attr);
            continue;
        }
        char sub[1024];
        snprintf(sub, sizeof sub, "%s/%s", path, attr->name);
        if (attr->type == SSH_FILEXFER_TYPE_DIRECTORY) {
            delete_dir_recursive(tab_idx, sub);
        } else {
            sftp_unlink(sftp, sub);
        }
        sftp_attributes_free(attr);
    }
    sftp_closedir(dir);
    if (sftp_rmdir(sftp, path) == SSH_OK) {
       log_status(T("已删除目录：%s", "Deleted folder: %s"), path);
        return true;
    }
    return false;
}

static bool g_del_file_dlg = false;
static char g_del_file_path[1024] = "";
static bool g_del_file_is_dir = false;
static void delete_file_dialog(cui_ctx *ui) {
    if (!g_del_file_dlg) return;
    cui_dialog(ui, T("确认删除", "Confirm delete"), &g_del_file_dlg, .width = 400) {
        cui_textf(ui, T("确定删除 %s 吗？", "Delete %s?"), g_del_file_path);
        if (g_del_file_is_dir) {
            cui_text(ui, T("（文件夹会连同其内容一起删除）",
                           "(Folder and its contents will be deleted)"),
                     .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, T("取消", "Cancel"), .variant = CUI_BUTTON_GHOST)) {
                g_del_file_dlg = false;
            }
            if (cui_button(ui, T("删除", "Delete"), .variant = CUI_BUTTON_DANGER)) {
                if (g_ssh_tab >= 0 && g_tab_sftp[g_ssh_tab] && g_del_file_path[0]) {
                    if (g_del_file_is_dir) {
                        delete_dir_recursive(g_ssh_tab, g_del_file_path);
                    } else {
                        if (sftp_unlink(g_tab_sftp[g_ssh_tab], g_del_file_path) == SSH_OK) {
                            log_status_toast(ui, CUI_TONE_SUCCESS, T("已删除：%s", "Deleted: %s"), g_del_file_path);
                        } else {
                            log_status_toast(ui, CUI_TONE_DANGER, T("删除失败：%s", "Delete failed: %s"), ssh_get_error(g_tab_ssh[g_ssh_tab]));
                        }
                    }
                    scan_remote(g_path);
                }
                g_del_file_dlg = false;
            }
        }
    }
}

typedef struct {
    int   item_count;
    char  items[16][64];
    int   actions[16];
    int   disabled[16];
    int   hover;
    float item_h;
    cui_color bg, border, text, text_disabled, hover_bg;
} ctx_menu_paint;

static void ctx_menu_paint_fn(cui_ctx *ui, cui_rect r, void *user) {
    ctx_menu_paint *m = user;
    /* 背景 + 边框 */
    cui_draw_rect_border(ui, r, CUI_CORNERS(6), m->bg, m->border, 1);

    float iy = r.y + 4;
    for (int k = 0; k < m->item_count; k++) {
        int act = m->actions[k];
        if (act == CTX_ACTION_SEPARATOR) {
            cui_draw_rect(ui,
                          (cui_rect){ r.x + 8, iy + m->item_h * 0.5f - 0.5f, r.w - 16, 1 },
                          CUI_CORNERS(0), m->border);
            iy += m->item_h * 0.5f;
            continue;
        }
        bool hv = (m->hover == k);
        if (hv) {
            cui_draw_rect(ui, (cui_rect){ r.x + 4, iy, r.w - 8, m->item_h },
                          CUI_CORNERS(4), m->hover_bg);
        }
        const char *icon = NULL;
        switch (act) {
        case CTX_ACTION_NEW_FILE: icon = CUI_ICON_FILE; break;
        case CTX_ACTION_NEW_DIR:  icon = CUI_ICON_FOLDER; break;
        case CTX_ACTION_COPY:     icon = CUI_ICON_COPY; break;
        case CTX_ACTION_CUT:      icon = CUI_ICON_SCISSORS; break;
        case CTX_ACTION_PASTE:    icon = CUI_ICON_CLIPBOARD; break;
        case CTX_ACTION_DOWNLOAD: icon = CUI_ICON_DOWNLOAD; break;
        case CTX_ACTION_UPLOAD:   icon = CUI_ICON_UPLOAD; break;
        case CTX_ACTION_DELETE:   icon = CUI_ICON_TRASH_2; break;
        case CTX_ACTION_RENAME:   icon = CUI_ICON_PENCIL; break;
        case CTX_ACTION_PROPS:    icon = CUI_ICON_INFO; break;
        }
        cui_color tc = m->disabled[k] ? m->text_disabled : m->text;
        if (act == CTX_ACTION_DELETE) tc = CUI_C(ui, DANGER);
        if (icon) {
            cui_draw_icon(ui, icon,
                          (cui_rect){ r.x + 10, iy + (m->item_h - 14) * 0.5f, 14, 14 },
                          14, tc);
        }
        float tx = r.x + 10 + 14 + 8;
        cui_draw_text(ui, CUI_FONT_BODY, 12,
                      (cui_vec2){ tx, iy + (m->item_h - 12) * 0.5f },
                      tc, m->items[k], -1);
        iy += m->item_h;
    }
}

static void file_ctx_dialog(cui_ctx *ui) {
    if (!g_fctx_open) return;
    if (g_fctx_target >= g_file_count) {   /* -1 表示空白菜单，放行 */
        g_fctx_open = false;
        return;
    }

    /* 计算菜单大小 */
    float item_h = 26;
    float menu_w = 180;
    float menu_h = 8;
    for (int k = 0; k < g_fctx_item_count; k++) {
        if (g_fctx_item_action[k] == CTX_ACTION_SEPARATOR)
            menu_h += item_h * 0.5f;
        else
            menu_h += item_h;
    }
    menu_h += 8;

    float vw = cui_viewport_size(ui).x;
    float vh = cui_viewport_size(ui).y;
    float mx = g_fctx_x;
    float my = g_fctx_y;
    if (mx + menu_w > vw) mx = vw - menu_w - 4;
    if (my + menu_h > vh) my = vh - menu_h - 4;
    if (mx < 4) mx = 4;
    if (my < 4) my = 4;

    /* ===== 全屏透明遮罩：拦截所有点击 ===== */
    cui_id backdrop_id = cui_make_id(ui, "ctx-menu-backdrop");
    int backdrop_node = cui__float_begin(ui, backdrop_id, 99, 0, CUI__PLACE_POINT, 0);
    ui->nodes[backdrop_node].fx = 0;
    ui->nodes[backdrop_node].fy = 0;
    ui->nodes[backdrop_node].w = CUI_PX(vw);
    ui->nodes[backdrop_node].h = CUI_PX(vh);
    ui->nodes[backdrop_node].flags |= CUI_NF_INTERACTIVE;
    cui__float_end(ui);

    /* 遮罩交互检测 */
    cui_interact backdrop_vr = cui__interact(ui, backdrop_id, 0);

    /* ===== 菜单 ===== */
    static ctx_menu_paint m;
    memset(&m, 0, sizeof m);
    m.item_count = g_fctx_item_count;
    for (int k = 0; k < g_fctx_item_count; k++) {
        snprintf(m.items[k], sizeof m.items[k], "%s", g_fctx_items[k]);
        m.actions[k] = g_fctx_item_action[k];
        m.disabled[k] = 0;
        if (g_fctx_item_action[k] == CTX_ACTION_PASTE && g_clip_path[0] == 0)
            m.disabled[k] = 1;
    }
    m.item_h = item_h;
    m.hover = -1;
    m.bg = cui_color_of(ui, CUI_COLOR_SURFACE_RAISED);
    m.border = cui_color_of(ui, CUI_COLOR_BORDER_STRONG);
    m.text = cui_color_of(ui, CUI_COLOR_FOREGROUND);
    m.text_disabled = cui_color_of(ui, CUI_COLOR_TEXT_MUTED);
    m.hover_bg = cui_color_of(ui, CUI_COLOR_HOVER);

    cui_id menu_id = cui_make_id(ui, "ctx-menu");
    int menu_node = cui__float_begin(ui, menu_id, 100, 0, CUI__PLACE_POINT, 0);
    ui->nodes[menu_node].fx = mx;
    ui->nodes[menu_node].fy = my;
    ui->nodes[menu_node].w = CUI_PX(menu_w);
    ui->nodes[menu_node].h = CUI_PX(menu_h);
    cui_canvas_result cr = cui_canvas(ui, "ctx-menu-canvas",
                                       CUI_FILL, CUI_FILL,
                                       ctx_menu_paint_fn, &m, sizeof m);
    cui__float_end(ui);

    /* hover */
    float iy = my + 4;
    int hover = -1;
    for (int k = 0; k < g_fctx_item_count; k++) {
        int act = g_fctx_item_action[k];
        float h = (act == CTX_ACTION_SEPARATOR) ? item_h * 0.5f : item_h;
        if (cr.hovered && ui->mx >= mx && ui->mx <= mx + menu_w
            && ui->my >= iy && ui->my <= iy + h && act != CTX_ACTION_SEPARATOR) {
            hover = k;
            break;
        }
        iy += h;
    }
    m.hover = hover;

    /* 点击处理 */
    bool inside_menu = (ui->mx >= mx && ui->mx <= mx + menu_w
                        && ui->my >= my && ui->my <= my + menu_h);

    /* 菜单内点击 */
    if (cr.clicked && inside_menu) {
        iy = my + 4;
        for (int k = 0; k < g_fctx_item_count; k++) {
            int act = g_fctx_item_action[k];
            float h = (act == CTX_ACTION_SEPARATOR) ? item_h * 0.5f : item_h;
            if (ui->mx >= mx && ui->mx <= mx + menu_w
                && ui->my >= iy && ui->my <= iy + h && act != CTX_ACTION_SEPARATOR) {
                int i = g_fctx_target;
                char remote_full[1024] = "";
                if (i >= 0 && i < g_file_count) {
                    if (g_path[0] && g_path[strlen(g_path) - 1] == '/')
                        snprintf(remote_full, sizeof remote_full, "%s%s", g_path, g_files[i].name);
                    else
                        snprintf(remote_full, sizeof remote_full, "%s/%s", g_path, g_files[i].name);
                }

                switch (act) {
                case CTX_ACTION_REFRESH:
                    scan_remote(g_path);
                    break;
                case CTX_ACTION_NEW_FILE:
                    g_newfile_dlg = true;
                    g_newfile_name[0] = 0;
                    break;
                case CTX_ACTION_NEW_DIR:
                    g_newdir_dlg = true;
                    g_newdir_name[0] = 0;
                    break;
                case CTX_ACTION_COPY:
                    if (i >= 0) {
                        snprintf(g_clip_path, sizeof g_clip_path, "%s", remote_full);
                        snprintf(g_clip_name, sizeof g_clip_name, "%s", g_files[i].name);
                        g_clip_is_cut = false;
                        log_status_toast(ui, CUI_TONE_SUCCESS,T("已复制：%s", "Copied: %s"), remote_full);
                    }
                    break;
                case CTX_ACTION_CUT:
                    if (i >= 0) {
                        snprintf(g_clip_path, sizeof g_clip_path, "%s", remote_full);
                        snprintf(g_clip_name, sizeof g_clip_name, "%s", g_files[i].name);
                        g_clip_is_cut = true;
                       log_status_toast(ui, CUI_TONE_SUCCESS,T("已剪切：%s", "Cut: %s"), remote_full);
                    }
                    break;
                case CTX_ACTION_PASTE:
                    if (g_clip_path[0]) {
                        snprintf(g_paste_name, sizeof g_paste_name, "%s", g_clip_name);
                        char src_dir[512], dst_dir[512];
                        snprintf(src_dir, sizeof src_dir, "%s", g_clip_path);
                        snprintf(dst_dir, sizeof dst_dir, "%s", g_path);
                        char *s1 = strrchr(src_dir, '/');
                        if (s1) *s1 = 0;
                        if (!strcmp(src_dir, dst_dir))
                            snprintf(g_paste_name, sizeof g_paste_name, "%s.bak", g_clip_name);
                        g_paste_dlg = true;
                    }
                    break;
                case CTX_ACTION_DOWNLOAD:
                    if (i >= 0) download_and_open(ui, remote_full, g_files[i].name, g_files[i].bytes, false);
                    break;
                case CTX_ACTION_UPLOAD:
#ifdef _WIN32
                    {
                        wchar_t file[1024] = L"";
                        OPENFILENAMEW ofn;
                        memset(&ofn, 0, sizeof ofn);
                        ofn.lStructSize = sizeof ofn;
                        ofn.lpstrFilter = L"所有文件 (*.*)\0*.*\0";
                        ofn.lpstrFile = file;
                        ofn.nMaxFile = sizeof file / sizeof file[0];
                        ofn.lpstrTitle = L"选择要上传的文件";
                        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                        if (GetOpenFileNameW(&ofn)) {
                            char local2[1024];
                            wchar_to_utf8(file, local2, sizeof local2);
                            const char *base2 = strrchr(local2, '\\');
                            if (!base2) base2 = strrchr(local2, '/');
                            base2 = base2 ? base2 + 1 : local2;
                            char remote2[1024];
                            if (g_path[0] && g_path[strlen(g_path)-1] == '/')
                                snprintf(remote2, sizeof remote2, "%s%s", g_path, base2);
                            else
                                snprintf(remote2, sizeof remote2, "%s/%s", g_path, base2);
                            upload_to_remote(ui, g_ssh_tab, local2, remote2);
                        }
                    }
#endif
                    break;
                case CTX_ACTION_DELETE:
                    if (i >= 0) {
                        snprintf(g_del_file_path, sizeof g_del_file_path, "%s", remote_full);
                        g_del_file_is_dir = g_files[i].is_dir;
                        g_del_file_dlg = true;
                    }
                    break;
                case CTX_ACTION_RENAME:
                    if (i >= 0) {
                        snprintf(g_rename_old, sizeof g_rename_old, "%s", remote_full);
                        snprintf(g_rename_new, sizeof g_rename_new, "%s", g_files[i].name);
                        g_rename_dlg = true;
                    }
                    break;
                case CTX_ACTION_PROPS:
                    if (i >= 0) {
                        g_props_row = g_files[i];
                        snprintf(g_props_path, sizeof g_props_path, "%s", remote_full);
                        g_props_dlg = true;
                    } else {
                        /* 目录属性：用当前 g_path 和 g_files[] */
                        snprintf(g_dir_props_path, sizeof g_dir_props_path, "%s", g_path);
                        g_dir_props_perm[0] = g_dir_props_owner[0] = g_dir_props_mtime[0] = 0;
                        g_dir_props_files = g_dir_props_dirs = 0;
                        g_dir_props_bytes = 0;
                        if (g_ssh_tab >= 0 && g_ssh_tab < g_tab_count && g_tab_sftp[g_ssh_tab]) {
                            sftp_attributes attr = sftp_stat(g_tab_sftp[g_ssh_tab], g_path);
                            if (attr) {
                                //snprintf(g_dir_props_perm, sizeof g_dir_props_perm, "%o", attr->permissions & 07777);
                                format_perm(attr->permissions, true, g_dir_props_perm, sizeof g_dir_props_perm); 
                                snprintf(g_dir_props_owner, sizeof g_dir_props_owner, "%s/%s",
                                         attr->owner ? attr->owner : "-",
                                         attr->group ? attr->group : "-");
                                time_t mt = (time_t)attr->mtime;
                                struct tm *tmv = localtime(&mt);
                                if (tmv) strftime(g_dir_props_mtime, sizeof g_dir_props_mtime, "%Y-%m-%d %H:%M", tmv);
                                sftp_attributes_free(attr);
                            }
                        }
                        for (int fi = 0; fi < g_file_count; fi++) {
                            if (g_files[fi].is_dir) g_dir_props_dirs++;
                            else { g_dir_props_files++; g_dir_props_bytes += g_files[fi].bytes; }
                        }
                        g_dir_props_dlg = true;
                    }
                    break;
                }
                g_fctx_open = false;
                break;
            }
            iy += h;
        }
    }
    /* 菜单外点击（遮罩被点击）→ 关闭菜单 */
    else if (backdrop_vr.pressed) {
        g_fctx_open = false;
    }

    /* Esc 关闭 */
    if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) {
        g_fctx_open = false;
    }
}

static void history_dialog(cui_ctx *ui) {
    if (!g_history_dlg) return;

    /* 当前会话 */
    int sess = -1;
    if (g_ssh_tab >= 0 && g_ssh_tab < g_tab_count)
        sess = g_tabs[g_ssh_tab].session_idx;
    else if (g_selected >= 0 && g_selected < g_session_count)
        sess = g_selected;

    ssh_session_cfg *cs = (sess >= 0 && sess < g_session_count) ? &g_sessions[sess] : NULL;
    int n = cs ? cs->nav_count : 0;

    cui_dialog(ui, T("历史路径", "Recent paths"), &g_history_dlg, .width = 560) {
        if (n == 0) {
            cui_text(ui, T("暂无历史", "No history"), .color = CUI_COLOR_TEXT_SECONDARY);
        } else {
            int remove_idx = -1;
            int nav_idx = -1;
            cui_scroll(ui, .id = "history-scroll", .height = CUI_PX(360)) {
                for (int i = 0; i < n; i++) {
                    cui_push_id_int(ui, i);

                    char id[64];
                    snprintf(id, sizeof id, "hist-row-%d", i);
                    cui_id row_id = cui_make_id(ui, id);
                    int row = cui__node_new(ui, row_id);
                    ui->nodes[row].w = CUI_FILL;
                    ui->nodes[row].h = CUI_PX(26);
                    cui__open(ui, row);
                        cui_row(ui, .width = CUI_FILL, .gap = 4, .pad_x = 4, .align = CUI_ALIGN_CENTER) {
                            cui_text(ui, cs->nav_history[i], .size = 12, .nowrap = true,
                                     .width = CUI_GROW(1));
                            if (cui_icon_button(ui, CUI_ICON_X,
                                                .tooltip = T("移除", "Remove"),
                                                .size = CUI_SIZE_SM)) {
                                remove_idx = i;
                            }
                        }
                    cui__close(ui);
                    cui__mark_interactive(ui, row, 0);

                    cui_interact vr = cui__interact(ui, row_id, 0);
                    static bool prev_pressed[64];
                    bool clicked_now = vr.pressed && !prev_pressed[i];
                    prev_pressed[i] = vr.pressed;
                    if (clicked_now) {
                        static double last_time[64];
                        static int last_idx[64];
                        double now = cui_time(ui);
                        if (last_idx[i] == i && now - last_time[i] < 0.35) {
                            last_idx[i] = -1;
                            nav_idx = i;
                        } else {
                            last_idx[i] = i;
                            last_time[i] = now;
                        }
                    }

                    cui_pop_id(ui);
                }
            }
            if (remove_idx >= 0 && cs) {
                for (int j = remove_idx; j < cs->nav_count - 1; j++)
                    memcpy(cs->nav_history[j], cs->nav_history[j+1], sizeof cs->nav_history[0]);
                cs->nav_count--;
                save_sessions();
            }
            if (nav_idx >= 0 && cs) {
                navigate_to(cs->nav_history[nav_idx]);
                g_history_dlg = false;
            }
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, T("清空", "Clear"), .variant = CUI_BUTTON_GHOST) && cs) {
                cs->nav_count = 0;
                save_sessions();
            }
            if (cui_button(ui, T("关闭", "Close"), .variant = CUI_BUTTON_PRIMARY)) {
                g_history_dlg = false;
            }
        }
    }
}
/* ← 新增：大小写不敏感子串匹配，needle 为空返回 true */
static bool str_contains_ci(const char *haystack, const char *needle) {
    if (!needle || !*needle) return true;
    if (!haystack) return false;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t k = 0;
        while (k < nlen && p[k] &&
               tolower((unsigned char)p[k]) == tolower((unsigned char)needle[k]))
            k++;
        if (k == nlen) return true;
    }
    return false;
}

static void format_perm(mode_t perms, bool is_dir, char *out, size_t cap) {
    if (cap < 11) { out[0] = 0; return; }
    out[0] = is_dir ? 'd' : '-';
    out[1] = (perms & 0400) ? 'r' : '-';
    out[2] = (perms & 0200) ? 'w' : '-';
    out[3] = (perms & 0100) ? 'x' : '-';
    out[4] = (perms & 0040) ? 'r' : '-';
    out[5] = (perms & 0020) ? 'w' : '-';
    out[6] = (perms & 0010) ? 'x' : '-';
    out[7] = (perms & 0004) ? 'r' : '-';
    out[8] = (perms & 0002) ? 'w' : '-';
    out[9] = (perms & 0001) ? 'x' : '-';
    out[10] = 0;

    /* setuid / setgid / sticky */
    if (perms & 04000) out[3] = (perms & 0100) ? 's' : 'S';
    if (perms & 02000) out[6] = (perms & 0010) ? 's' : 'S';
    if (perms & 01000) out[9] = (perms & 0001) ? 't' : 'T';
}
/* ================================================================== */
/* 右栏下：文件树 + 文件表                                             */
/* ================================================================== */
static void files_panel(cui_ctx *ui) {
    bool remote = (g_ssh_tab >= 0 && g_ssh_tab < g_tab_count && g_tab_sftp[g_ssh_tab]);
    /* ← 新增：拖放上传 */
    int drop_n = cui_dropped_file_count(ui);
    if (drop_n > 0) {
        if (!remote) {
            log_status_toast(ui, CUI_TONE_WARNING,
                             T("请先连接 SFTP 再拖入文件", "Connect SFTP before dropping files"));
        } else if (g_ul.active || g_upload_dir_active) {
            log_status_toast(ui, CUI_TONE_WARNING,
                             T("已有上传进行中，稍后再拖入", "An upload is already running"));
        } else {
            /* 展开到队列 */
            g_upload_dir_file_count_total = 0;
            g_upload_dir_file_count_done = 0;
            g_upload_dir_next_idx = 0;
            uint64_t total_bytes = 0;
            for (int k = 0; k < drop_n; k++) {
                const char *p = cui_dropped_file(ui, k);
                if (p && p[0]) drop_collect_path(ui, p, g_path, &total_bytes);
            }
            if (g_upload_dir_file_count_total == 0) {
                log_status_toast(ui, CUI_TONE_WARNING,
                                 T("没有可上传的文件", "No files to upload"));
            } else if (!check_remote_space_for_upload(ui, g_ssh_tab, g_path, total_bytes)) {
                g_upload_dir_file_count_total = 0;
            } else {
                /* 队列启动 */
                g_upload_dir_active = true;
                snprintf(g_upload_dir_name, sizeof g_upload_dir_name, "%s",
                         drop_n == 1 ? T("拖入的文件", "Dropped file") : T("拖入的文件", "Dropped files"));
                snprintf(g_upload_dir_remote, sizeof g_upload_dir_remote, "%s", g_path);
                /* 启动第一个 */
                if (g_upload_dir_next_idx < g_upload_dir_file_count_total) {
                    int k0 = g_upload_dir_next_idx++;
                    upload_to_remote(ui, g_ssh_tab,
                                     g_upload_dir_files[k0], g_upload_dir_remotes[k0]);
                }
                log_status_toast(ui, CUI_TONE_INFO,
                                 T("开始上传 %d 个文件到 %s", "Uploading %d files to %s"),
                                 g_upload_dir_file_count_total, g_path);
            }
        }
    }

    if (!remote) {
        cui_text(ui, T("未连接到 SFTP。打开一个会话后显示远程文件。",
                       "Not connected to SFTP. Open a session to browse remote files."),
                 .color = CUI_COLOR_TEXT_SECONDARY);
        return;
    }
    if (g_last_fm_tab != g_ssh_tab) {
        g_last_fm_tab = g_ssh_tab;
        g_tree_count = 0;
        snprintf(g_path, sizeof g_path, "/");
        scan_remote("/");
        tree_ensure_path("/");
        tree_load_children("/");
    }

    /* 工具栏 */
    cui_row(ui, .width = CUI_FILL, .gap = 4, .align = CUI_ALIGN_CENTER) {
        cui_box(ui, .width = CUI_GROW(1)) {
            int r = cui_input(ui, "fm-path", g_path, sizeof g_path, .placeholder = "/");
            if (r & CUI_SUBMITTED) {
                navigate_to(g_path);
            }
        }
        if (cui_icon_button(ui, CUI_ICON_CLOCK, .tooltip = T("历史", "History"))) {
            g_history_dlg = true;
        }
        /* ← 新增：文件搜索框 */
        cui_box(ui, .width = CUI_PX(180)) {
            cui_search(ui, "fm-search", g_fm_search, sizeof g_fm_search,
                       .placeholder = T("搜索文件...", "Search files..."));
        }        
        if (cui_icon_button(ui, CUI_ICON_REFRESH_CW, .tooltip = T("刷新", "Refresh")))
            scan_remote(g_path);
        if (cui_icon_button(ui, CUI_ICON_ARROW_UP, .tooltip = T("上一级", "Up"))) {
            char up[512];
            snprintf(up, sizeof up, "%s", g_path);
            char *slash = strrchr(up, '/');
            if (slash == up) { up[1] = 0; scan_remote(up); }
            else if (slash && slash != up) { *slash = 0; scan_remote(up); }
        }
        /* 下载：选中的文件 */
        if (cui_icon_button(ui, CUI_ICON_DOWNLOAD, .tooltip = T("下载选中文件", "Download selected"))) {
            if (g_file_selected >= 0 && g_file_selected < g_file_count && !g_files[g_file_selected].is_dir) {
                char remote[512];
                if (g_path[0] && g_path[strlen(g_path) - 1] == '/')
                    snprintf(remote, sizeof remote, "%s%s", g_path, g_files[g_file_selected].name);
                else
                    snprintf(remote, sizeof remote, "%s/%s", g_path, g_files[g_file_selected].name);
                download_and_open(ui, remote, g_files[g_file_selected].name, g_files[g_file_selected].bytes, false);
            } else {
                log_status_toast(ui, CUI_TONE_WARNING, T("请先选中一个文件再下载", "Select a file first"));
            }
        }
        /* 上传：打开文件选择器 */
        if (cui_icon_button(ui, CUI_ICON_UPLOAD, .tooltip = T("上传文件", "Upload file"))) {
#ifdef _WIN32
            wchar_t file[1024] = L"";
            OPENFILENAMEW ofn;
            memset(&ofn, 0, sizeof ofn);
            ofn.lStructSize = sizeof ofn;
            ofn.lpstrFilter = L"所有文件 (*.*)\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = sizeof file / sizeof file[0];
            ofn.lpstrTitle = L"选择要上传的文件";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) {
                char local[1024];
                wchar_to_utf8(file, local, sizeof local);
                const char *base = strrchr(local, '\\');
                if (!base) base = strrchr(local, '/');
                base = base ? base + 1 : local;
                char remote[1024];
                if (g_path[0] && g_path[strlen(g_path) - 1] == '/')
                    snprintf(remote, sizeof remote, "%s%s", g_path, base);
                else
                    snprintf(remote, sizeof remote, "%s/%s", g_path, base);
                if (upload_to_remote(ui, g_ssh_tab, local, remote)) {
                    scan_remote(g_path);
                }
            }
#else
            log_status_toast(ui, CUI_TONE_WARNING, T("上传仅支持 Windows", "Upload is Windows-only"));
#endif
        }
        /* 文件传输列表 */
        if (cui_icon_button(ui, CUI_ICON_LIST, .tooltip = T("传输列表", "Transfers"))) {
            g_transfers_dlg = true;
        }
    }

    cui_split_opts fso = { ._ = 0, .vertical = false, .min_first = 140.0f, .min_second = 300.0f };
    cui_split_begin(ui, "file-split", &g_file_split, &fso);

    /* 左侧递归树：cui__interact 按 id 检测交互 + 单击切换/双击刷新 */
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .border = 1, .radius = 6, .bg = CUI_COLOR_SURFACE) {
        cui_scroll(ui, .id = "tree-scroll", .height = CUI_FILL, .pad = 6) {
            for (int k = 0; k < g_tree_count; k++) {
                tree_entry *te = &g_tree[k];

                /* 检查所有祖先是否展开 */
                int visible = 1;
                char ancestor[512];
                snprintf(ancestor, sizeof ancestor, "%s", te->path);
                for (;;) {
                    char *slash = strrchr(ancestor, '/');
                    if (!slash || slash == ancestor) break;
                    *slash = 0;
                    int ai = tree_find(ancestor);
                    if (ai >= 0 && !g_tree[ai].open) { visible = 0; break; }
                }
                if (!visible) continue;

                /* 创建交互行节点 */
                char id[600];
                snprintf(id, sizeof id, "tree-row-%d", k);
                cui_id row_id = cui_make_id(ui, id);
                int row = cui__node_new(ui, row_id);
                ui->nodes[row].w = CUI_FILL;
                ui->nodes[row].h = CUI_PX(24);
                cui__open(ui, row);
                    cui_row(ui, .width = CUI_FILL, .gap = 2, .align = CUI_ALIGN_CENTER) {
                        if (te->depth > 0)
                            cui_box(ui, .width = CUI_PX(te->depth * 16.0f)) {}
                        cui_icon(ui, te->open ? CUI_ICON_CHEVRON_DOWN : CUI_ICON_CHEVRON_RIGHT,
                                 14, CUI_COLOR_TEXT_SECONDARY);
                        cui_text(ui, te->name, .nowrap = true, .size = 12, .width = CUI_GROW(1));
                    }
                cui__close(ui);
                cui__mark_interactive(ui, row, 0);

                /* 交互检测：单帧按下触发 */
                cui_interact vr = cui__interact(ui, row_id, 0);
                if (vr.hovered) cui__set_cursor(ui, CUI_CURSOR_HAND);
                static bool prev_pressed[512];
                bool clicked_now = vr.pressed && !prev_pressed[k];
                prev_pressed[k] = vr.pressed;
                if (!clicked_now) continue;

                /* 双击检测 */
                static double last_click_time[512];
                static int last_clicked[512];
                double now = cui_time(ui);
                if (last_clicked[k] == k && now - last_click_time[k] < 0.35) {
                    /* 双击：只刷新右侧，open 不变 */
                    last_clicked[k] = -1;
                    scan_remote(te->path);
                    tree_ensure_path(te->path);
                    tree_load_children(te->path);
                } else {
                    /* 单击：切换 open */
                    te->open = !te->open;
                    if (te->open) tree_load_children(te->path);
                    last_clicked[k] = k;
                    last_click_time[k] = now;
                }
            }
        }
    }
    
    cui_split_next(ui);

    /* 右侧文件表：cui__interact 按 id 检测交互 + 图标 + 多列 */
    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .border = cui_file_drag_active(ui) ? 2.0f : 1.0f,
            .border_color = cui_file_drag_active(ui) ? CUI_COLOR_ACCENT : CUI_COLOR_BORDER, .radius = 6, .bg = CUI_COLOR_SURFACE) {
        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 4, .gap = 0, .id = "files-list-wrap") {
            /* 表头（可排序） */
            cui_row(ui, .width = CUI_FILL, .gap = 4, .pad_y = 4, .pad_left = 10) {
                sortable_header(ui, "files", T("名称", "Name"), 0, true, CUI_TEXT_LEFT, &g_file_sort, 0);
                sortable_header(ui, "files", T("大小", "Size"), 64, false, CUI_TEXT_LEFT, &g_file_sort, 1);
                sortable_header(ui, "files", T("类型", "Type"), 64, false, CUI_TEXT_LEFT, &g_file_sort, 2);
                sortable_header(ui, "files", T("修改时间", "Modified"), 96, false, CUI_TEXT_LEFT, &g_file_sort, 3);
                sortable_header(ui, "files", T("权限", "Perm"), 96, false, CUI_TEXT_LEFT, &g_file_sort, 4);
                sortable_header(ui, "files", T("用户/组", "Owner"), 96, false, CUI_TEXT_LEFT, &g_file_sort, 5);
            }
            hline(ui);

            int fidx[4096];
            int fn = g_file_count < 4096 ? g_file_count : 4096;
            sort_indices(fidx, fn, g_file_sort, g_files, sizeof(file_row), file_cmp_ff);

            /* ← 新增：按 g_fm_search 过滤，得到可见下标 */
            int vidx[4096];
            int vn = 0;
            if (g_fm_search[0]) {
                for (int k = 0; k < fn; k++) {
                    int i = fidx[k];
                    if (str_contains_ci(g_files[i].name, g_fm_search))
                        vidx[vn++] = i;
                }
            } else {
                for (int k = 0; k < fn; k++) vidx[vn++] = fidx[k];
            }
            
            bool any_row_clicked = false;
            cui_scroll(ui, .id = "files-scroll", .height = CUI_GROW(1)) {
                for (int k = 0; k < vn; k++) {
                    int i = vidx[k];

                    /* 创建交互行节点 */
                    char id[64];
                    snprintf(id, sizeof id, "frow-%d", i);
                    cui_id row_id = cui_make_id(ui, id);
                    int row = cui__node_new(ui, row_id);
                    ui->nodes[row].w = CUI_FILL;
                    ui->nodes[row].h = CUI_PX(24);
                    /* 选中高亮 */
                    if (g_file_selected == i)
                        ui->nodes[row].bg = cui_alpha(CUI_C(ui, ACCENT), 0.2f);
                    cui__open(ui, row);
                    cui_row(ui, .width = CUI_FILL, .gap = 4, .align = CUI_ALIGN_CENTER, .pad_left = 10) {
                        /* 名称列：图标 + 文本 */
                        cui_box(ui, .width = CUI_GROW(1)) {
                            cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
                                cui_icon(ui, g_files[i].is_dir ? CUI_ICON_FOLDER : CUI_ICON_FILE, 14,
                                         g_files[i].is_dir ? CUI_COLOR_ACCENT : CUI_COLOR_TEXT_SECONDARY);
                                cui_text(ui, g_files[i].name, .nowrap = true, .size = 12);
                            }
                        }
                        cui_box(ui, .width = CUI_PX(64)) {
                            cui_text(ui, g_files[i].size, .nowrap = true, .size = 12);
                        }
                        cui_box(ui, .width = CUI_PX(64)) {
                            cui_text(ui, g_files[i].is_dir ? T("文件夹", "dir") : T("文件", "file"),
                                     .nowrap = true, .size = 12);
                        }
                        cui_box(ui, .width = CUI_PX(96)) {
                            cui_text(ui, g_files[i].mtime, .nowrap = true, .size = 12);
                        }
                        cui_box(ui, .width = CUI_PX(96)) {
                            cui_text(ui, g_files[i].perm, .nowrap = true, .size = 12);
                        }
                        cui_box(ui, .width = CUI_PX(96)) {
                            cui_text(ui, g_files[i].owner, .nowrap = true, .size = 12);
                        }
                    }
                    cui__close(ui);
                    cui__mark_interactive(ui, row, 0);

                    /* 交互检测：单帧按下 */
                    cui_interact vr = cui__interact(ui, row_id, 0);
                    if (vr.hovered) cui__set_cursor(ui, CUI_CURSOR_HAND);
                    static bool prev_pressed[4096];
                    bool clicked_now = vr.pressed && !prev_pressed[i];
                    prev_pressed[i] = vr.pressed;

                    /* 右键检测（在 continue 之前） */
                    if (vr.hovered && cui_mouse_clicked(ui, CUI_MOUSE_RIGHT)) {
                        any_row_clicked = true;
                        g_fctx_target = i;
                        g_fctx_x = ui->mx;
                        g_fctx_y = ui->my;
                        g_fctx_open = true;
                        g_fctx_hover = -1;
                        g_file_selected = i;

                        /* 构造菜单项 */
                        g_fctx_item_count = 0;
                        snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", "");
                        //g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_SEPARATOR;
                        snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", T("刷新", "Refresh"));
                        g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_REFRESH;
                        g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_SEPARATOR;
                        snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", T("新建文件", "New file"));
                        g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_NEW_FILE;
                        snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", T("新建文件夹", "New folder"));
                        g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_NEW_DIR;
                        snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", "");
                        g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_SEPARATOR;
                        snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", T("复制", "Copy"));
                        g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_COPY;
                        snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", T("剪切", "Cut"));
                        g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_CUT;
                        snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", T("粘贴", "Paste"));
                        g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_PASTE;
                        snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", "");
                        g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_SEPARATOR;
                        if (!g_files[i].is_dir) {
                            snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", T("下载", "Download"));
                            g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_DOWNLOAD;
                        }
                        snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", T("上传", "Upload"));
                        g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_UPLOAD;
                        snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", "");
                        g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_SEPARATOR;
                        snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", T("删除", "Delete"));
                        g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_DELETE;
                        snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", T("重命名", "Rename"));
                        g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_RENAME;
                        snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", T("属性", "Properties"));
                        g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_PROPS;
                    }

                    if (!clicked_now) continue;

                    /* 单击选中 */
                    any_row_clicked = true;
                    g_file_selected = i;

                    /* 双击检测 */
                    static double last_click_time[4096];
                    static int last_clicked[4096];
                    double now = cui_time(ui);
                    if (last_clicked[i] == i && now - last_click_time[i] < 0.35) {
                        last_clicked[i] = -1;
                        /* 双击：文件夹进入 / 文件下载 */
                        char sub[512];
                        if (g_path[0] && g_path[strlen(g_path) - 1] == '/')
                            snprintf(sub, sizeof sub, "%s%s", g_path, g_files[i].name);
                        else
                            snprintf(sub, sizeof sub, "%s/%s", g_path, g_files[i].name);
                        if (g_files[i].is_dir) {
                            scan_remote(sub);
                            tree_ensure_path(sub);
                            tree_load_children(sub);
                        } else {
                            download_and_open(ui, sub, g_files[i].name, g_files[i].bytes, true);
                        }
                    } else {
                        last_clicked[i] = i;
                        last_click_time[i] = now;
                    }
                }
                if (vn == 0) {
                    cui_text(ui, T("没有匹配的文件", "No matching files"),
                             .color = CUI_COLOR_TEXT_SECONDARY);
                }
                cui_box(ui, .width = CUI_FILL, .height = CUI_PX(50)) {}
            }
            /* ← 空白检测移到 column 内、scroll 之后 */
            if (!any_row_clicked && cui_mouse_clicked(ui, CUI_MOUSE_LEFT)) {
                cui_rect sr = cui_get_rect(ui, cui_make_id(ui, "files-list-wrap"));
                cui_vec2 mp = cui_mouse_pos(ui);
                if (sr.w > 0 && sr.h > 0 &&
                    mp.x >= sr.x && mp.x <= sr.x + sr.w &&
                    mp.y >= sr.y + 26 && mp.y <= sr.y + sr.h) {
                    g_file_selected = -1;
                }
            }
            /* ← 新增：空白处右键菜单 */
            if (!any_row_clicked && !g_fctx_open && cui_mouse_clicked(ui, CUI_MOUSE_RIGHT)) {
                cui_rect sr = cui__prev_rect(ui, cui_make_id(ui, "files-list-wrap"));
                cui_vec2 mp = cui_mouse_pos(ui);
                if (sr.w > 0 && sr.h > 0 &&
                    mp.x >= sr.x && mp.x <= sr.x + sr.w &&
                    mp.y >= sr.y + 26 && mp.y <= sr.y + sr.h) {
                    g_fctx_target = -1;
                    g_fctx_x = ui->mx;
                    g_fctx_y = ui->my;
                    g_fctx_open = true;
                    g_fctx_hover = -1;
                    g_file_selected = -1;

                    g_fctx_item_count = 0;
                    snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", T("刷新", "Refresh"));
                    g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_REFRESH;
                    g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_SEPARATOR;
                    snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", T("新建文件", "New file"));
                    g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_NEW_FILE;
                    snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", T("新建文件夹", "New folder"));
                    g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_NEW_DIR;
                    snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", "");
                    g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_SEPARATOR;
                    snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", T("粘贴", "Paste"));
                    g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_PASTE;
                    snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", T("上传", "Upload"));
                    g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_UPLOAD;
                    snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", "");
                    g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_SEPARATOR;
                    snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", T("属性", "Properties"));
                    g_fctx_item_action[g_fctx_item_count++] = CTX_ACTION_PROPS;
                    snprintf(g_fctx_items[g_fctx_item_count], 64, "%s", "");
                }
            }
        }
    }

    cui_split_end(ui);
}

/* ================================================================== */
/* 弹窗                                                                */
/* ================================================================== */

static void new_edit_dialog(cui_ctx *ui) {
    bool *open = g_new_dlg ? &g_new_dlg : &g_edit_dlg;
    if (!*open) return;
    cui_dialog(ui, g_new_dlg ? T("新建会话", "New session") : T("编辑会话", "Edit session"), open, .width = 460) {
        cui_input(ui, "dlg-name", dlg_name, sizeof dlg_name, .label = T("名称", "Name"));
        cui_input(ui, "dlg-host", dlg_host, sizeof dlg_host, .label = T("IP", "IP"));
        char port_buf[16]; snprintf(port_buf, sizeof port_buf, "%d", dlg_port);
        if (cui_input(ui, "dlg-port", port_buf, sizeof port_buf, .label = T("端口", "Port")))
            dlg_port = atoi(port_buf);
        cui_input(ui, "dlg-user", dlg_user, sizeof dlg_user, .label = T("用户名", "Username"));
        cui_input(ui, "dlg-pass", dlg_pass, sizeof dlg_pass, .label = T("密码", "Password"), .password = true);
        cui_dialog_footer(ui) {
            if (cui_button(ui, T("取消", "Cancel"), .variant = CUI_BUTTON_GHOST)) *open = false;
            if (cui_button(ui, T("确定", "OK"), .variant = CUI_BUTTON_PRIMARY)) {
                if (g_new_dlg) {
                    if (g_session_count < MAX_SESSIONS) {
                        ssh_session_cfg *s = &g_sessions[g_session_count++];
                        memset(s, 0, sizeof *s);
                        snprintf(s->name, sizeof s->name, "%s", dlg_name);
                        snprintf(s->host, sizeof s->host, "%s", dlg_host);
                        s->port = dlg_port;
                        snprintf(s->user, sizeof s->user, "%s", dlg_user);
                        encrypt_pass(dlg_pass, s->pass_enc);
                    }
                } else if (g_ctx_target >= 0 && g_ctx_target < g_session_count) {
                    ssh_session_cfg *s = &g_sessions[g_ctx_target];
                    snprintf(s->name, sizeof s->name, "%s", dlg_name);
                    snprintf(s->host, sizeof s->host, "%s", dlg_host);
                    s->port = dlg_port;
                    snprintf(s->user, sizeof s->user, "%s", dlg_user);
                    encrypt_pass(dlg_pass, s->pass_enc);
                }
                save_sessions();
                *open = false;
            }
        }
    }
}

static void delete_dialog(cui_ctx *ui) {
    if (!g_del_dlg) return;
    cui_dialog(ui, T("确认删除", "Confirm delete"), &g_del_dlg, .width = 380) {
        cui_text(ui, T("确定删除这个会话吗？", "Delete this session?"), .size = 13);
        cui_dialog_footer(ui) {
            if (cui_button(ui, T("取消", "Cancel"), .variant = CUI_BUTTON_GHOST)) g_del_dlg = false;
            if (cui_button(ui, T("删除", "Delete"), .variant = CUI_BUTTON_DANGER)) {
                if (g_ctx_target >= 0 && g_ctx_target < g_session_count) {
                    for (int i = g_ctx_target; i < g_session_count - 1; i++)
                        g_sessions[i] = g_sessions[i+1];
                    g_session_count--;
                    if (g_selected == g_ctx_target) g_selected = -1;
                    save_sessions();
                }
                g_del_dlg = false;
            }
        }
    }
}

static void retry_dialog(cui_ctx *ui) {
    if (!g_retry_dlg || g_retry_idx < 0) return;
    ssh_session_cfg *s = &g_sessions[g_retry_idx];
    static char r_user[MAX_USER], r_pass[MAX_PASS];
    if (!r_user[0]) snprintf(r_user, sizeof r_user, "%s", s->user);
    cui_dialog(ui, T("登录", "Sign in"), &g_retry_dlg, .width = 420) {
        cui_textf(ui, "%s@%s:%d", s->name, s->host, s->port);
        cui_input(ui, "r-user", r_user, sizeof r_user, .label = T("用户名", "Username"));
        cui_input(ui, "r-pass", r_pass, sizeof r_pass, .label = T("密码", "Password"), .password = true);
        cui_checkbox(ui, T("记住登录信息", "Remember me"), &dlg_remember);
        cui_dialog_footer(ui) {
            if (cui_button(ui, T("取消", "Cancel"), .variant = CUI_BUTTON_GHOST)) {
                g_retry_dlg = false; g_retry_idx = -1;
                r_user[0] = r_pass[0] = 0;
            }
            if (cui_button(ui, T("确定", "OK"), .variant = CUI_BUTTON_PRIMARY)) {
                char err[256] = "";
                ssh_session conn = NULL;
                bool ok = do_connect(&conn, s->host, s->port, r_user, r_pass, err, sizeof err);
                if (ok) {
                    snprintf(s->user, sizeof s->user, "%s", r_user);
                    if (dlg_remember) encrypt_pass(r_pass, s->pass_enc);
                    save_sessions();
                    s->connected = true; s->auth_failed = false;
                    open_tab_with_ssh(g_retry_idx, conn);
                    log_status_toast(ui, CUI_TONE_SUCCESS,
                                     T("已连接 %s", "Connected to %s"), s->name);
                } else {
                    log_status_toast(ui, CUI_TONE_DANGER,
                                     T("重连失败：%s", "Reconnect failed: %s"), err);
                }
                g_retry_dlg = false; g_retry_idx = -1;
                r_user[0] = r_pass[0] = 0;
            }
        }
    }
}

static void log_dialog(cui_ctx *ui) {
    if (!g_log_dlg) return;
    cui_dialog(ui, T("状态栏日志", "Status log"), &g_log_dlg, .width = 720) {
        cui_scroll(ui, .id = "log-scroll", .height = CUI_PX(400)) {
            for (int k = 0; k < g_log_count; k++) {
                int idx = (g_log_head - 1 - k + LOG_HISTORY * 2) % LOG_HISTORY;
                cui_text(ui, g_log_history[idx], .size = 11, .font = CUI_FONT_MONO, .nowrap = true);
            }
            if (g_log_count == 0)
                cui_text(ui, T("暂无日志", "No log yet"), .color = CUI_COLOR_TEXT_SECONDARY);
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, T("清空", "Clear"), .variant = CUI_BUTTON_GHOST)) {
                g_log_count = 0; g_log_head = 0;
            }
            if (cui_button(ui, T("关闭", "Close"), .variant = CUI_BUTTON_PRIMARY))
                g_log_dlg = false;
        }
    }
}

static void settings_dialog(cui_ctx *ui) {
    if (!g_settings_dlg) return;
    if (g_settings_dl_buf[0] == 0 && g_settings_ed_buf[0] == 0) {
        snprintf(g_settings_dl_buf, sizeof g_settings_dl_buf, "%s", g_download_dir);
        snprintf(g_settings_ed_buf, sizeof g_settings_ed_buf, "%s", g_editor_path);
        snprintf(g_settings_key_buf, sizeof g_settings_key_buf, "%s", g_ssh_key_path);
        snprintf(g_settings_noext_buf, sizeof g_settings_noext_buf, "%s", g_text_noext);
        snprintf(g_settings_exts_buf, sizeof g_settings_exts_buf, "%s", g_text_exts);
        snprintf(g_settings_term_size_buf, sizeof g_settings_term_size_buf,
                 "%.0f", g_term_font_size);   /* ← 只在打开时初始化一次 */
    }
    cui_dialog(ui, T("设置", "Settings"), &g_settings_dlg, .width = 620) {
        cui_text(ui, T("终端字体大小", "Terminal font size"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        if (cui_input(ui, "dlg-term-size", g_settings_term_size_buf,
                      sizeof g_settings_term_size_buf, .placeholder = "13")) {
            /* 只在用户确认输入时（失焦/回车）更新，或每帧更新都行 */
            float v = (float)atof(g_settings_term_size_buf);
            if (v >= 8 && v <= 32) g_term_font_size = v;
        }
        /* ← 新增：SSH 私钥 */
        cui_text(ui, T("SSH 私钥路径（全局，留空则只用密码）",
                       "SSH private key (global, empty = password only)"),
                 .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .width = CUI_GROW(1)) {
                cui_input(ui, "dlg-ssh-key", g_settings_key_buf, sizeof g_settings_key_buf,
                          .placeholder = T("例如 C:\\Users\\me\\.ssh\\id_ed25519",
                                           "e.g. C:\\Users\\me\\.ssh\\id_ed25519"));
            }
            if (cui_button(ui, T("浏览...##key", "Browse...##key"), .variant = CUI_BUTTON_OUTLINE,
                           .icon = CUI_ICON_FOLDER_OPEN)) {
                browse_for_ssh_key(g_settings_key_buf, sizeof g_settings_key_buf);
            }
        }
        cui_gap(ui, 8);
        /* 下载目录 */
        cui_text(ui, T("下载目录", "Download directory"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .width = CUI_GROW(1)) {
                cui_input(ui, "dlg-download", g_settings_dl_buf, sizeof g_settings_dl_buf,
                          .placeholder = T("例如 C:\\Downloads", "e.g. C:\\Downloads"));
            }
            if (cui_button(ui, T("浏览...##dl", "Browse...##dl"), .variant = CUI_BUTTON_OUTLINE,
                           .icon = CUI_ICON_FOLDER_OPEN)) {
                browse_for_folder(g_settings_dl_buf, sizeof g_settings_dl_buf);
            }
        }
        cui_gap(ui, 8);

        /* 外部编辑器 */
        cui_text(ui, T("外部编辑器（完整路径，例如 notepad.exe）",
                       "External editor (full path, e.g. notepad.exe)"),
                 .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        cui_row(ui, .width = CUI_FILL, .gap = 6, .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .width = CUI_GROW(1)) {
                cui_input(ui, "dlg-editor", g_settings_ed_buf, sizeof g_settings_ed_buf,
                          .placeholder = T("例如 C:\\Windows\\notepad.exe",
                                           "e.g. C:\\Windows\\notepad.exe"));
            }
            if (cui_button(ui, T("浏览...##ed", "Browse...##ed"), .variant = CUI_BUTTON_OUTLINE,
                           .icon = CUI_ICON_FOLDER_OPEN)) {
                browse_for_editor(g_settings_ed_buf, sizeof g_settings_ed_buf);
            }
        }
        cui_gap(ui, 8);
        
        
        
        /* 文本文件名（无后缀） */
        cui_text(ui, T("无后缀的文本文件名（逗号分隔，不区分大小写）",
                       "Text filenames without extension (comma-separated, case-insensitive)"),
                 .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        cui_textarea(ui, "dlg-noext", g_settings_noext_buf, sizeof g_settings_noext_buf,
                     .placeholder = T("例如 Makefile,README,LICENSE",
                                      "e.g. Makefile,README,LICENSE"),
                     .height = 72); 
        cui_gap(ui, 8);

        /* 文本文件后缀 */
        cui_text(ui, T("文本文件后缀（逗号分隔，不带 .，不区分大小写）",
                       "Text file extensions (comma-separated, no dot, case-insensitive)"),
                 .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        cui_textarea(ui, "dlg-exts", g_settings_exts_buf, sizeof g_settings_exts_buf,
                     .placeholder = T("例如 txt,md,c,h,cpp,json,py",
                                      "e.g. txt,md,c,h,cpp,json,py"),
                     .height = 72);

        cui_dialog_footer(ui) {
            if (cui_button(ui, T("取消", "Cancel"), .variant = CUI_BUTTON_GHOST)) {
                g_settings_dlg = false;
                g_settings_dl_buf[0] = 0;
                g_settings_ed_buf[0] = 0;
                 g_settings_key_buf[0] = 0;
                g_settings_noext_buf[0] = 0;
                g_settings_exts_buf[0] = 0;
            }
            if (cui_button(ui, T("保存", "Save"), .variant = CUI_BUTTON_PRIMARY)) {
                float v = (float)atof(g_settings_term_size_buf);
                if (v >= 8 && v <= 32) g_term_font_size = v;
                snprintf(g_ssh_key_path, sizeof g_ssh_key_path, "%s", g_settings_key_buf);
                snprintf(g_download_dir, sizeof g_download_dir, "%s", g_settings_dl_buf);
                snprintf(g_editor_path, sizeof g_editor_path, "%s", g_settings_ed_buf);
                for (char *p = g_settings_noext_buf; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
                for (char *p = g_settings_exts_buf; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
                snprintf(g_text_noext, sizeof g_text_noext, "%s", g_settings_noext_buf);
                snprintf(g_text_exts, sizeof g_text_exts, "%s", g_settings_exts_buf);
                save_sessions();
                log_status_toast(ui, CUI_TONE_SUCCESS, T("设置已保存", "Settings saved"));
                g_settings_dlg = false;
                g_settings_dl_buf[0] = 0;
                g_settings_ed_buf[0] = 0;
                g_settings_key_buf[0] = 0; 
                g_settings_noext_buf[0] = 0;
                g_settings_exts_buf[0] = 0;
            }
        }
    }
}

static void transfers_dialog(cui_ctx *ui) {
    if (!g_transfers_dlg) return;
    cui_dialog(ui, T("文件传输", "File transfers"), &g_transfers_dlg, .width = 1000) {
        if (g_transfer_count == 0) {
            cui_text(ui, T("暂无传输记录", "No transfers"), .color = CUI_COLOR_TEXT_SECONDARY);
        } else {
            /* 表头 */
            cui_row(ui, .width = CUI_FILL, .gap = 4, .pad_y = 4, .align = CUI_ALIGN_CENTER) {
                cui_box(ui, .width = CUI_PX(24)) {}
                cui_box(ui, .width = CUI_PX(60)) { cui_text(ui, T("类型", "Type"), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY); }
                cui_box(ui, .width = CUI_GROW(1))  { cui_text(ui, T("名称", "Name"), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY); }
                cui_box(ui, .width = CUI_PX(180)) { cui_text(ui, T("源", "Source"), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY); }
                cui_box(ui, .width = CUI_PX(180)) { cui_text(ui, T("目的", "Target"), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY); }
                cui_box(ui, .width = CUI_PX(80))  { cui_text(ui, T("状态", "Status"), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY); }
                cui_box(ui, .width = CUI_PX(120)) { cui_text(ui, T("进度", "Progress"), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY); }
                cui_box(ui, .width = CUI_PX(70))  {}
            }
            hline(ui);

            int remove_idx = -1;
            cui_scroll(ui, .id = "transfers-scroll", .height = CUI_PX(400)) {
                for (int k = g_transfer_count - 1; k >= 0; k--) {
                    cui_push_id_int(ui, k);
                    transfer_entry *t = &g_transfers[k];
                    cui_row(ui, .width = CUI_FILL, .gap = 4, .pad_y = 3, .align = CUI_ALIGN_CENTER) {
                        /* 状态图标 */
                        const char *icon = CUI_ICON_CIRCLE;
                        cui_color color = CUI_COLOR_TEXT_SECONDARY;
                        switch (t->status) {
                        case XFER_PENDING: icon = CUI_ICON_CLOCK; color = CUI_COLOR_TEXT_SECONDARY; break;
                        case XFER_ACTIVE:  icon = CUI_ICON_LOADER_CIRCLE; color = CUI_COLOR_ACCENT; break;
                        case XFER_DONE:    icon = CUI_ICON_CIRCLE_CHECK; color = CUI_COLOR_SUCCESS; break;
                        case XFER_FAILED:  icon = CUI_ICON_CIRCLE_X; color = CUI_COLOR_DANGER; break;
                        }
                        cui_icon(ui, icon, 16, color);

                        /* 类型 */
                        cui_box(ui, .width = CUI_PX(60)) {
                            cui_text(ui,
                                     t->type == XFER_DOWNLOAD ? T("下载", "Download") :
                                     (t->type == XFER_UPLOAD ? T("上传", "Upload") : T("上传目录", "Upload dir")),
                                     .size = 11, .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
                        }

                        /* 名称 */
                        cui_box(ui, .width = CUI_GROW(1)) {
                            const char *name = strrchr(t->local, '\\');
                            if (!name) name = strrchr(t->local, '/');
                            name = name ? name + 1 : t->local;
                            cui_text(ui, name, .size = 12, .nowrap = true);
                        }

                        /* 源 */
                        cui_box(ui, .width = CUI_PX(180)) {
                            const char *src = t->type == XFER_DOWNLOAD ? t->remote : t->local;
                            char s[64];
                            snprintf(s, sizeof s, "%s", src);
                            if (strlen(s) > 25) snprintf(s, sizeof s, "...%s", src + strlen(src) - 22);
                            cui_text(ui, s, .size = 10, .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
                        }

                        /* 目的 */
                        cui_box(ui, .width = CUI_PX(180)) {
                            const char *dst = t->type == XFER_DOWNLOAD ? t->local : t->remote;
                            char s[64];
                            snprintf(s, sizeof s, "%s", dst);
                            if (strlen(s) > 25) snprintf(s, sizeof s, "...%s", dst + strlen(dst) - 22);
                            cui_text(ui, s, .size = 10, .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
                        }

                        /* 状态 */
                        cui_box(ui, .width = CUI_PX(80)) {
                            const char *st = T("等待", "Pending");
                            if (t->status == XFER_ACTIVE) st = T("进行中", "Active");
                            else if (t->status == XFER_DONE) st = T("完成", "Done");
                            else if (t->status == XFER_FAILED) st = T("失败", "Failed");
                            cui_text(ui, st, .size = 11, .nowrap = true);
                        }

                        /* 进度 */
                        cui_box(ui, .width = CUI_PX(120)) {
                            if (t->status == XFER_ACTIVE) {
                                char pct[32];
                                snprintf(pct, sizeof pct, "%.0f%%",
                                         t->total > 0 ? t->done * 100.0 / t->total : 0);
                                cui_progress(ui,
                                             t->total > 0 ? (float)((double)t->done / (double)t->total) : 0,
                                             .height = 4);
                                cui_text(ui, pct, .size = 10, .nowrap = true);
                                
                            } else if (t->status == XFER_DONE) {
                                cui_text(ui, "100%", .size = 10, .color = CUI_COLOR_SUCCESS);
                            } else if (t->status == XFER_FAILED) {
                                cui_text(ui, t->err[0] ? t->err : T("失败", "Failed"),
                                         .size = 10, .color = CUI_COLOR_DANGER, .nowrap = true);
                            }
                        }

                        /* 打开文件夹 / 取消 / 移除 */
                        if (t->status == XFER_DONE) {
                            if (cui_icon_button(ui, CUI_ICON_FOLDER_OPEN, .tooltip = T("打开位置", "Open location"))) {
                                const char *dir = t->local;
                        #ifdef _WIN32
                                char args[1200];
                                snprintf(args, sizeof args, "/select,\"%s\"", dir);
                                ShellExecuteA(NULL, "open", "explorer.exe", args, NULL, SW_SHOWNORMAL);
                        #endif
                            }
                        } else if (t->status == XFER_ACTIVE || t->status == XFER_PENDING) {
                            /* 取消按钮 */
                            if (cui_icon_button(ui, CUI_ICON_CIRCLE_X, .tooltip = T("取消", "Cancel"))) {
                                /* 判断这个任务是不是当前正在进行的下载/上传 */
                                if (t->type == XFER_DOWNLOAD && g_dl.active && g_dl.transfer_idx == k) {
                                    /* 取消当前下载 */
                                    if (g_dl.out) { fclose(g_dl.out); g_dl.out = NULL; }
                                    if (g_dl.file) { sftp_close(g_dl.file); g_dl.file = NULL; }
                                    g_dl.active = false;
                                    g_transfers[k].status = XFER_FAILED;
                                    snprintf(g_transfers[k].err, sizeof g_transfers[k].err,
                                             T("已取消", "Cancelled"));
                                    log_status_toast(ui, CUI_TONE_WARNING, T("下载已取消：%s", "Download cancelled: %s"), g_dl.local_path);
                                    save_transfers();
                                } else if (t->type == XFER_UPLOAD && g_ul.active && g_ul.transfer_idx == k) {
                                    /* 取消当前上传 */
                                    if (g_ul.in) { fclose(g_ul.in); g_ul.in = NULL; }
                                    if (g_ul.file) { sftp_close(g_ul.file); g_ul.file = NULL; }
                                    g_ul.active = false;
                                    g_transfers[k].status = XFER_FAILED;
                                    snprintf(g_transfers[k].err, sizeof g_transfers[k].err,
                                             T("已取消", "Cancelled"));
                                    log_status_toast(ui, CUI_TONE_WARNING, T("上传已取消：%s", "Upload cancelled: %s"), g_ul.local_path);
                                    save_transfers();
                                } else {
                                    /* 不是当前任务（比如排队中，或历史 ACTIVE 残留）：直接标失败 */
                                    g_transfers[k].status = XFER_FAILED;
                                    snprintf(g_transfers[k].err, sizeof g_transfers[k].err,
                                             T("已取消", "Cancelled"));
                                    save_transfers();
                                }
                            }
                        } else {
                            cui_box(ui, .width = CUI_PX(24)) {}
                        }
                        if (cui_icon_button(ui, CUI_ICON_X, .tooltip = T("移除", "Remove"))) {
                            remove_idx = k;
                        }
                    }
                    cui_pop_id(ui);
                }
            }
            if (remove_idx >= 0) {
                for (int j = remove_idx; j < g_transfer_count - 1; j++)
                    memcpy(&g_transfers[j], &g_transfers[j + 1], sizeof(g_transfers[0]));
                g_transfer_count--;
                save_transfers();
            }
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, T("清空", "Clear"), .variant = CUI_BUTTON_GHOST)) {
                g_transfer_count = 0;
                save_transfers();
            }
            if (cui_button(ui, T("关闭", "Close"), .variant = CUI_BUTTON_PRIMARY)) {
                g_transfers_dlg = false;
            }
        }
    }
}

static void newfile_dialog(cui_ctx *ui) {
    if (!g_newfile_dlg) return;
    cui_dialog(ui, T("新建文件", "New file"), &g_newfile_dlg, .width = 400) {
        cui_input(ui, "nf-name", g_newfile_name, sizeof g_newfile_name, .label = T("文件名", "Name"));
        cui_dialog_footer(ui) {
            if (cui_button(ui, T("取消", "Cancel"), .variant = CUI_BUTTON_GHOST)) g_newfile_dlg = false;
            if (cui_button(ui, T("确定", "OK"), .variant = CUI_BUTTON_PRIMARY)) {
                if (g_newfile_name[0] && g_ssh_tab >= 0 && g_tab_sftp[g_ssh_tab]) {
                    char remote[1024];
                    if (g_path[0] && g_path[strlen(g_path)-1] == '/')
                        snprintf(remote, sizeof remote, "%s%s", g_path, g_newfile_name);
                    else
                        snprintf(remote, sizeof remote, "%s/%s", g_path, g_newfile_name);
                    sftp_file f = sftp_open(g_tab_sftp[g_ssh_tab], remote,
                                            O_WRONLY | O_CREAT | O_EXCL, 0644);
                    if (f) {
                        sftp_close(f);
                        log_status_toast(ui, CUI_TONE_SUCCESS, T("已创建文件：%s", "File created: %s"), remote);
                        scan_remote(g_path);
                    } else {
                        log_status_toast(ui, CUI_TONE_DANGER, T("创建失败：%s", "Create failed: %s"), ssh_get_error(g_tab_ssh[g_ssh_tab]));
                    }
                }
                g_newfile_dlg = false;
                g_newfile_name[0] = 0;
            }
        }
    }
}

static void newdir_dialog(cui_ctx *ui) {
    if (!g_newdir_dlg) return;
    cui_dialog(ui, T("新建文件夹", "New folder"), &g_newdir_dlg, .width = 400) {
        cui_input(ui, "nd-name", g_newdir_name, sizeof g_newdir_name, .label = T("文件夹名", "Name"));
        cui_dialog_footer(ui) {
            if (cui_button(ui, T("取消", "Cancel"), .variant = CUI_BUTTON_GHOST)) g_newdir_dlg = false;
            if (cui_button(ui, T("确定", "OK"), .variant = CUI_BUTTON_PRIMARY)) {
                if (g_newdir_name[0] && g_ssh_tab >= 0 && g_tab_sftp[g_ssh_tab]) {
                    char remote[1024];
                    if (g_path[0] && g_path[strlen(g_path)-1] == '/')
                        snprintf(remote, sizeof remote, "%s%s", g_path, g_newdir_name);
                    else
                        snprintf(remote, sizeof remote, "%s/%s", g_path, g_newdir_name);
                    int rc = sftp_mkdir(g_tab_sftp[g_ssh_tab], remote, 0755);
                    if (rc == SSH_OK) {
                        log_status_toast(ui, CUI_TONE_SUCCESS, T("已创建文件夹：%s", "Folder created: %s"), remote);
                        scan_remote(g_path);
                    } else {
                        log_status_toast(ui, CUI_TONE_DANGER, T("创建失败：%s", "Create failed: %s"), ssh_get_error(g_tab_ssh[g_ssh_tab]));
                    }
                }
                g_newdir_dlg = false;
                g_newdir_name[0] = 0;
            }
        }
    }
}

static void props_dialog(cui_ctx *ui) {
    if (!g_props_dlg) return;
    cui_dialog(ui, T("属性", "Properties"), &g_props_dlg, .width = 480) {
        cui_column(ui, .width = CUI_FILL, .gap = 8) {
            cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, g_props_row.is_dir ? CUI_ICON_FOLDER : CUI_ICON_FILE,
                         32, g_props_row.is_dir ? CUI_COLOR_ACCENT : CUI_COLOR_TEXT_SECONDARY);
                cui_column(ui, .width = CUI_GROW(1), .gap = 2) {
                    cui_text(ui, g_props_row.name, .font = CUI_FONT_MEDIUM);
                    cui_text(ui, g_props_row.is_dir ? T("文件夹", "Folder") : T("文件", "File"),
                             .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
                }
            }
            hline(ui);
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                cui_text(ui, T("路径", "Path"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_text(ui, g_props_path, .size = 12, .nowrap = true);
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                cui_text(ui, T("大小", "Size"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_text(ui, g_props_row.size, .size = 12);
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                cui_text(ui, T("修改时间", "Modified"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_text(ui, g_props_row.mtime, .size = 12);
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                cui_text(ui, T("权限", "Permissions"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_text(ui, g_props_row.perm, .size = 12);
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                cui_text(ui, T("所有者", "Owner"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_text(ui, g_props_row.owner, .size = 12);
            }
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, T("关闭", "Close"), .variant = CUI_BUTTON_PRIMARY)) {
                g_props_dlg = false;
            }
        }
    }
}

static void dir_props_dialog(cui_ctx *ui) {
    if (!g_dir_props_dlg) return;
    cui_dialog(ui, T("目录属性", "Folder properties"), &g_dir_props_dlg, .width = 520) {
        cui_column(ui, .width = CUI_FILL, .gap = 8) {
            cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, CUI_ICON_FOLDER, 32, CUI_COLOR_ACCENT);
                cui_column(ui, .width = CUI_GROW(1), .gap = 2) {
                    const char *base = strrchr(g_dir_props_path, '/');
                    cui_text(ui, base && base[1] ? base + 1 : "/", .font = CUI_FONT_MEDIUM);
                    cui_text(ui, T("文件夹", "Folder"), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
                }
            }
            hline(ui);
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                cui_text(ui, T("路径", "Path"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_text(ui, g_dir_props_path, .size = 12, .nowrap = true);
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                cui_text(ui, T("当前层文件数", "Files here"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_textf(ui, "%d", g_dir_props_files);
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                cui_text(ui, T("当前层文件夹数", "Folders here"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_textf(ui, "%d", g_dir_props_dirs);
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                cui_text(ui, T("当前层文件大小", "Size of files here"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                if (g_dir_props_bytes < 1024) cui_textf(ui, "%llu B", (unsigned long long)g_dir_props_bytes);
                else if (g_dir_props_bytes < 1024*1024) cui_textf(ui, "%.1f KB", g_dir_props_bytes / 1024.0);
                else if (g_dir_props_bytes < 1024ULL*1024*1024) cui_textf(ui, "%.1f MB", g_dir_props_bytes / (1024.0*1024.0));
                else cui_textf(ui, "%.2f GB", g_dir_props_bytes / (1024.0*1024.0*1024.0));
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                cui_text(ui, T("权限", "Permissions"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_text(ui, g_dir_props_perm[0] ? g_dir_props_perm : "-", .size = 12);
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                cui_text(ui, T("所有者", "Owner"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_text(ui, g_dir_props_owner[0] ? g_dir_props_owner : "-", .size = 12);
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                cui_text(ui, T("修改时间", "Modified"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_text(ui, g_dir_props_mtime[0] ? g_dir_props_mtime : "-", .size = 12);
            }
            cui_text(ui, T("注：大小只统计当前层文件，不含子目录。",
                           "Note: size counts files in this folder only, not subfolders."),
                     .size = 11, .color = CUI_COLOR_TEXT_MUTED);
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, T("关闭", "Close"), .variant = CUI_BUTTON_PRIMARY))
                g_dir_props_dlg = false;
        }
    }
}

static void paste_dialog(cui_ctx *ui) {
    if (!g_paste_dlg) return;
    cui_dialog(ui, T("粘贴", "Paste"), &g_paste_dlg, .width = 450) {
        cui_textf(ui, T("源：%s", "Source: %s"), g_clip_path);
        cui_textf(ui, T("目标目录：%s", "Target: %s"), g_path);
        cui_input(ui, "paste-name", g_paste_name, sizeof g_paste_name,
                  .label = T("文件名", "Name"));
        cui_dialog_footer(ui) {
            if (cui_button(ui, T("取消", "Cancel"), .variant = CUI_BUTTON_GHOST)) {
                g_paste_dlg = false;
                g_paste_name[0] = 0;
            }
            if (cui_button(ui, T("确定", "OK"), .variant = CUI_BUTTON_PRIMARY)) {
                if (g_paste_name[0] && g_ssh_tab >= 0 && g_tab_sftp[g_ssh_tab]) {
                    char dest[1024];
                    if (g_path[0] && g_path[strlen(g_path)-1] == '/')
                        snprintf(dest, sizeof dest, "%s%s", g_path, g_paste_name);
                    else
                        snprintf(dest, sizeof dest, "%s/%s", g_path, g_paste_name);

                    bool is_dir = remote_path_is_dir(g_ssh_tab, g_clip_path);
                    if (g_clip_is_cut) {
                        /* 剪切：rename */
                        if (sftp_rename(g_tab_sftp[g_ssh_tab], g_clip_path, dest) == SSH_OK) {
                            log_status_toast(ui, CUI_TONE_SUCCESS, T("已移动：%s → %s", "Moved: %s → %s"), g_clip_path, dest);
                            g_clip_path[0] = 0;
                            scan_remote(g_path);
                        } else {
                            log_status_toast(ui, CUI_TONE_DANGER, T("移动失败：%s", "Move failed: %s"), ssh_get_error(g_tab_ssh[g_ssh_tab]));
                        }
                    } else {
                        /* 复制 */
                        if (is_dir) {
                            copy_dir_remote(g_ssh_tab, g_clip_path, dest);
                        } else {
                            copy_file_remote(g_ssh_tab, g_clip_path, dest);
                        }
                        scan_remote(g_path);
                        log_status_toast(ui, CUI_TONE_SUCCESS, T("已复制到：%s", "Copied to: %s"), dest);
                    }
                }
                g_paste_dlg = false;
                g_paste_name[0] = 0;
            }
        }
    }
}

static void about_dialog(cui_ctx *ui) {
    if (!g_about_dlg) return;
    cui_dialog(ui, T("关于 SSH Terminal", "About SSH Terminal"), &g_about_dlg, .width = 560) {
        cui_scroll(ui, .id = "about-scroll", .height = CUI_PX(480)) {
            cui_column(ui, .width = CUI_FILL, .gap = 12) {
                cui_text(ui, "SSH Terminal", .font = CUI_FONT_DISPLAY, .size = 24);
                cui_text(ui, "v1.0.0", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_separator(ui);
                cui_text(ui, T("SSH 远程终端与文件管理器", "SSH remote terminal and file manager"),
                         .size = 13);
                cui_gap(ui, 8);
                cui_text(ui, T("功能：", "Features:"), .size = 12, .font = CUI_FONT_MEDIUM);
                static const char *features_zh[] = {
                    "• 多会话 SSH 连接（密码 / 私钥）",
                    "• 交互式终端（shell 通道，支持 nano / vim 快捷键）",
                    "• 实时统计（CPU/内存/网络/磁盘/进程）",
                    "• SFTP 远程文件浏览与管理（树 + 文件表）",
                    "• 文件搜索过滤、拖拽上传",
                    "• 文件上传/下载（异步分块，进度显示，空间检查）",
                    "• 复制/剪切/粘贴/删除/重命名/文件与目录属性",
                    "• 命令面板（多行命令，一键执行）",
                    "• 端口转发（本地转发，配置随会话保存）",
                    "• 导航历史持久化（每会话最近 20 条路径）",
                    "• 下载后编辑自动回传",
                    "• 明暗主题、中英文切换、设置持久化",
                    NULL
                };
                static const char *features_en[] = {
                    "• Multi-session SSH (password / private key)",
                    "• Interactive shell (nano / vim shortcuts)",
                    "• Real-time stats (CPU/RAM/Net/Disk/Proc)",
                    "• SFTP remote file browsing and management (tree + table)",
                    "• File search filter, drag-and-drop upload",
                    "• Async upload/download with progress and space check",
                    "• Copy/Cut/Paste/Delete/Rename, file and folder properties",
                    "• Command panel (multi-line commands, one click)",
                    "• Port forwarding (local forward, saved per session)",
                    "• Persistent navigation history (last 20 paths per session)",
                    "• Auto upload after editing a downloaded file",
                    "• Light/dark theme, Chinese/English, persistent settings",
                    NULL
                };
                const char **features = (lang == 'z') ? features_zh : features_en;
                for (int i = 0; features[i]; i++)
                    cui_text(ui, features[i], .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);

                cui_gap(ui, 12);
                cui_separator(ui);
                cui_text(ui, T("联系方式", "Contact"), .size = 12, .font = CUI_FONT_MEDIUM);
                cui_text(ui, T("微信：xsoft2005", "WeChat: xsoft2005"),
                         .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_text(ui, T("Email：qb_xu@126.com", "Email: qb_xu@126.com"),
                         .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);

                cui_gap(ui, 12);
                cui_separator(ui);
                cui_text(ui, T("欢迎捐赠，感谢支持！", "Donations welcome. Thank you!"),
                         .size = 12, .font = CUI_FONT_MEDIUM);
                cui_text(ui, T("如果这个工具对你有帮助，欢迎通过微信或邮件联系作者。",
                               "If this tool helps you, feel free to reach out via WeChat or email."),
                         .size = 11, .color = CUI_COLOR_TEXT_MUTED);

                cui_gap(ui, 8);
                cui_text(ui, T("基于 CUI 即时模式 GUI 库 + libssh",
                               "Built with CUI immediate-mode GUI + libssh"),
                         .size = 11, .color = CUI_COLOR_TEXT_MUTED);
            }
        }
        cui_dialog_footer(ui) {
            if (cui_button(ui, T("关闭", "Close"), .variant = CUI_BUTTON_PRIMARY))
                g_about_dlg = false;
        }
    }
}

static void cmd_add_dialog(cui_ctx *ui) {
    if (!g_cmd_add_dlg) return;
    bool is_edit = (g_cmd_edit_idx >= 0);
    cui_dialog(ui, is_edit ? T("编辑命令", "Edit command") : T("添加命令", "Add command"),
               &g_cmd_add_dlg, .width = 600) {
        cui_text(ui, T("名称", "Name"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        cui_input(ui, "cmd-name", g_cmd_add_name, sizeof g_cmd_add_name, .placeholder = T("命令名称", "Command name"));

        cui_gap(ui, 8);
        cui_text(ui, T("命令", "Command"), .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        // cui_input(ui, "cmd-body", g_cmd_add_body, sizeof g_cmd_add_body,
        //           .placeholder = T("输入命令，可多行", "Enter command, multi-line allowed"),
        //           .height = 120);
        cui_code_editor(ui, "cmd-body",g_cmd_add_body, sizeof g_cmd_add_body,
                     .lang = "shell",
                     .height = 110,
                     .no_line_numbers = true,
                     .placeholder = T("输入命令，可多行", "Enter command, multi-line"));

        cui_gap(ui, 8);
        /* 参数插入提示 + 按钮 */
        cui_text(ui, T("插入参数（动态生成命令）", "Insert parameter (dynamic command)"),
                 .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
        cui_row(ui, .width = CUI_FILL, .gap = 4, .wrap = true) {
            for (int n = 1; n <= 5; n++) {
                char label[32];
                snprintf(label, sizeof label, T("参数%d", "Param %d"), n);
                if (cui_button(ui, label, .variant = CUI_BUTTON_OUTLINE, .size = CUI_SIZE_SM)) {
                    /* 在命令文本框光标位置插入 [p#n 参数名] */
                    /* 但 cui_input 没有"光标位置"API，只能在末尾插入 */
                    char ins[64];
                    snprintf(ins, sizeof ins, "[p#%d 参数名]", n);
                    size_t cur_len = strlen(g_cmd_add_body);
                    size_t ins_len = strlen(ins);
                    if (cur_len + ins_len < sizeof g_cmd_add_body - 1) {
                        memcpy(g_cmd_add_body + cur_len, ins, ins_len);
                        g_cmd_add_body[cur_len + ins_len] = 0;
                    }
                }
            }
        }

        cui_gap(ui, 8);
        /* 末尾添加回车符 CR */
        cui_checkbox(ui, T("末尾添加回车符 CR", "Append CR at end"), &g_cmd_add_cr);

        cui_dialog_footer(ui) {
            if (cui_button(ui, T("取消", "Cancel"), .variant = CUI_BUTTON_GHOST)) {
                g_cmd_add_dlg = false;
                g_cmd_edit_idx = -1;
            }
            if (cui_button(ui, T("确定", "OK"), .variant = CUI_BUTTON_PRIMARY)) {
                if (g_cmd_add_name[0] && g_cmd_add_body[0]) {
                    if (is_edit && g_cmd_edit_idx >= 0 && g_cmd_edit_idx < g_cmd_count) {
                        cmd_entry *c = &g_cmds[g_cmd_edit_idx];
                        snprintf(c->name, sizeof c->name, "%s", g_cmd_add_name);
                        snprintf(c->command, sizeof c->command, "%s", g_cmd_add_body);
                        c->add_cr = g_cmd_add_cr;
                    } else if (g_cmd_count < MAX_CMDS) {
                        cmd_entry *c = &g_cmds[g_cmd_count++];
                        snprintf(c->name, sizeof c->name, "%s", g_cmd_add_name);
                        snprintf(c->command, sizeof c->command, "%s", g_cmd_add_body);
                        c->created = time(NULL);
                        c->add_cr = g_cmd_add_cr;
                    }
                    save_cmds();
                }
                g_cmd_add_dlg = false;
                g_cmd_edit_idx = -1;
            }
        }
    }
}

static void right_panel(cui_ctx *ui) {
    cui_split_opts vso = { ._ = 0, .vertical = true, .min_first = 200, .min_second = 160 };
    cui_split_begin(ui, "right-split", &g_v_split, &vso);
    terminals_panel(ui);
    if (g_show_fm_panel) {
        cui_split_next(ui);
        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 4) {
            static const char *lz[] = { "文件", "命令", "常用指令" };
            static const char *le[] = { "Files", "Commands", "Common" };
            cui_tabs_ex(ui, "fm-tabs", lang == 'z' ? lz : le, 3, &g_fm_tab, NULL);
            if (g_fm_tab == 0)      files_panel(ui);
            else if (g_fm_tab == 1) commands_panel(ui);
            else                    linux_cmds_panel(ui);
        }
    }
    cui_split_end(ui);
}

static void rename_dialog(cui_ctx *ui) {
    if (!g_rename_dlg) return;
    cui_dialog(ui, T("重命名", "Rename"), &g_rename_dlg, .width = 450) {
        cui_textf(ui, T("原：%s", "Old: %s"), g_rename_old);
        cui_input(ui, "rename-new", g_rename_new, sizeof g_rename_new, .label = T("新名称", "New name"));
        cui_dialog_footer(ui) {
            if (cui_button(ui, T("取消", "Cancel"), .variant = CUI_BUTTON_GHOST)) g_rename_dlg = false;
            if (cui_button(ui, T("确定", "OK"), .variant = CUI_BUTTON_PRIMARY)) {
                if (g_rename_new[0] && g_ssh_tab >= 0 && g_tab_sftp[g_ssh_tab]) {
                    char dst[1024];
                    if (g_path[0] && g_path[strlen(g_path)-1] == '/')
                        snprintf(dst, sizeof dst, "%s%s", g_path, g_rename_new);
                    else
                        snprintf(dst, sizeof dst, "%s/%s", g_path, g_rename_new);
                    if (sftp_rename(g_tab_sftp[g_ssh_tab], g_rename_old, dst) == SSH_OK) {
                        log_status_toast(ui, CUI_TONE_SUCCESS, T("已重命名：%s → %s", "Renamed: %s → %s"), g_rename_old, dst);
                        scan_remote(g_path);
                    } else {
                        log_status_toast(ui, CUI_TONE_DANGER, T("重命名失败：%s", "Rename failed: %s"), ssh_get_error(g_tab_ssh[g_ssh_tab]));
                    }
                }
                g_rename_dlg = false;
            }
        }
    }
}

static void forwards_dialog(cui_ctx *ui) {
    if (!g_forwards_dlg) return;

    int sess = -1;
    if (g_ssh_tab >= 0 && g_ssh_tab < g_tab_count)
        sess = g_tabs[g_ssh_tab].session_idx;
    if (sess < 0 || sess >= g_session_count) {
        cui_dialog(ui, T("端口转发", "Port forwarding"), &g_forwards_dlg, .width = 640) {
            cui_text(ui, T("没有打开的会话", "No open session"),
                     .color = CUI_COLOR_TEXT_SECONDARY);
            cui_dialog_footer(ui) {
                if (cui_button(ui, T("关闭", "Close"), .variant = CUI_BUTTON_PRIMARY))
                    g_forwards_dlg = false;
            }
        }
        return;
    }

    ssh_session_cfg *cs = &g_sessions[sess];
    int tab = g_ssh_tab;

    cui_dialog(ui, T("端口转发", "Port forwarding"), &g_forwards_dlg, .width = 760) {
        /* 标题行：两段文本 + 右侧“添加隧道”按钮，底部对齐 */
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_END, .gap = 2) {
            cui_column(ui, .width = CUI_GROW(1), .gap = 2) {
                cui_textf(ui, T("会话：%s（修改会保存到会话配置）",
                                "Session: %s (changes are saved to the session)"),
                          cs->name);
                cui_text(ui, T("本地转发：本机端口 → 远端可访问的地址。",
                               "Local forward: local port → address the remote can reach."),
                         .size = 11, .color = CUI_COLOR_TEXT_SECONDARY);
            }
            if (cui_button(ui, T("添加隧道", "Add tunnel"),
                           .icon = CUI_ICON_PLUS, .variant = CUI_BUTTON_OUTLINE,
                           .size = CUI_SIZE_SM)) {
                if (cs->fwd_count < MAX_FWD_CFG) {
                    forward_cfg *fc = &cs->fwd[cs->fwd_count++];
                    memset(fc, 0, sizeof *fc);
                    snprintf(fc->local_host, sizeof fc->local_host, "127.0.0.1");
                    fc->local_port = 8080;
                    snprintf(fc->remote_host, sizeof fc->remote_host, "localhost");
                    fc->remote_port = 80;
                    fc->active = false;
                    save_sessions();
                }
            }
        }
        //cui_gap(ui, 2);
        hline(ui);
        /* 表头 */
        cui_row(ui, .width = CUI_FILL, .gap = 4, .pad_y = 4, .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .width = CUI_PX(28)) {}
            cui_box(ui, .width = CUI_GROW(1)) { cui_text(ui, T("本地地址", "Local host"), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY); }
            cui_box(ui, .width = CUI_PX(64))  { cui_text(ui, T("端口", "Port"), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY); }
            cui_box(ui, .width = CUI_GROW(1)) { cui_text(ui, T("远端地址", "Remote host"), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY); }
            cui_box(ui, .width = CUI_PX(64))  { cui_text(ui, T("端口", "Port"), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY); }
            cui_box(ui, .width = CUI_PX(80))  { cui_text(ui, T("状态", "Status"), .size = 11, .color = CUI_COLOR_TEXT_SECONDARY); }
            cui_box(ui, .width = CUI_PX(36))  {}
        }
        hline(ui);

        int del_idx = -1;
        bool changed = false;
        for (int i = 0; i < cs->fwd_count; i++) {
            forward_cfg *fc = &cs->fwd[i];
            cui_push_id_int(ui, i);

            /* 状态文本 */
            const char *status = T("已停止", "Stopped");
            cui_color   sc = CUI_COLOR_TEXT_SECONDARY;
            for (int g = 0; g < g_forward_count; g++) {
                port_forward *pf = &g_forwards[g];
                if (pf->running && pf->tab_idx == tab && pf->sess_idx == sess && pf->cfg_idx == i) {
                    status = T("监听中", "Listening");
                    sc = CUI_COLOR_SUCCESS;
                    if (pf->err[0]) { status = pf->err; sc = CUI_COLOR_DANGER; }
                    break;
                }
            }

            cui_row(ui, .width = CUI_FILL, .gap = 4, .pad_y = 2, .align = CUI_ALIGN_CENTER) {
                /* 启用 */
                bool was_active = fc->active;
                cui_checkbox(ui, "##en", &fc->active);
                if (was_active != fc->active) {
                    /* 先停旧的 */
                    for (int g = 0; g < g_forward_count; g++) {
                        port_forward *pf = &g_forwards[g];
                        if (pf->running && pf->tab_idx == tab && pf->sess_idx == sess && pf->cfg_idx == i) {
                            stop_forward(g);
                            break;
                        }
                    }
                    if (fc->active) start_forward(tab, sess, i);
                    changed = true;
                }

                /* 本地地址 */
                char lh_buf[64]; snprintf(lh_buf, sizeof lh_buf, "%s", fc->local_host);
                if (cui_input(ui, "##lh", lh_buf, sizeof lh_buf, .placeholder = "127.0.0.1")) {
                    snprintf(fc->local_host, sizeof fc->local_host, "%s", lh_buf);
                    changed = true;
                }
                /* 本地端口 */
                char lp_buf[16]; snprintf(lp_buf, sizeof lp_buf, "%d", fc->local_port);
                if (cui_input(ui, "##lp", lp_buf, sizeof lp_buf, .placeholder = "8080")) {
                    int v = atoi(lp_buf);
                    if (v > 0 && v < 65536) { fc->local_port = v; changed = true; }
                }
                /* 远端地址 */
                char rh_buf[128]; snprintf(rh_buf, sizeof rh_buf, "%s", fc->remote_host);
                if (cui_input(ui, "##rh", rh_buf, sizeof rh_buf, .placeholder = "localhost")) {
                    snprintf(fc->remote_host, sizeof fc->remote_host, "%s", rh_buf);
                    changed = true;
                }
                /* 远端端口 */
                char rp_buf[16]; snprintf(rp_buf, sizeof rp_buf, "%d", fc->remote_port);
                if (cui_input(ui, "##rp", rp_buf, sizeof rp_buf, .placeholder = "80")) {
                    int v = atoi(rp_buf);
                    if (v > 0 && v < 65536) { fc->remote_port = v; changed = true; }
                }

                /* 状态 */
                cui_box(ui, .width = CUI_PX(80)) {
                    cui_text(ui, status, .size = 11, .color = sc, .nowrap = true);
                }

                /* 删除 */
                if (cui_icon_button(ui, CUI_ICON_TRASH_2,
                                    .tooltip = T("删除", "Delete"), .size = CUI_SIZE_SM)) {
                    del_idx = i;
                }
            }
            cui_pop_id(ui);
        }

        if (del_idx >= 0) {
            /* 先停运行时 */
            for (int g = 0; g < g_forward_count; g++) {
                port_forward *pf = &g_forwards[g];
                if (pf->running && pf->tab_idx == tab && pf->sess_idx == sess && pf->cfg_idx == del_idx)
                    stop_forward(g);
            }
            /* 从会话配置移除 */
            for (int j = del_idx; j < cs->fwd_count - 1; j++)
                cs->fwd[j] = cs->fwd[j + 1];
            cs->fwd_count--;
            /* 修正运行时 cfg_idx */
            for (int g = 0; g < g_forward_count; g++) {
                port_forward *pf = &g_forwards[g];
                if (pf->sess_idx == sess && pf->cfg_idx > del_idx) pf->cfg_idx--;
            }
            changed = true;
        }

        if (changed) save_sessions();

        cui_dialog_footer(ui) {
            if (cui_button(ui, T("关闭", "Close"), .variant = CUI_BUTTON_PRIMARY))
                g_forwards_dlg = false;
        }
    }
}

/* 画"左右分隔"图标 */
static void split_lp_icon_paint(cui_ctx *ui, cui_node *n, void *data) {
    (void)data;
    cui_rect r = n->rect;
    /* 内边距 8 */
    float x0 = r.x + 8, y0 = r.y + 8;
    float w = r.w - 16, h = r.h - 16;
    //cui_color col = cui_color_of(ui, CUI_COLOR_FOREGROUND);
    cui_color col = cui_alpha(CUI_C(ui, FOREGROUND), 0.9f);
    /* 左右竖线 */
    cui_draw_rect_border(ui, (cui_rect){ x0, y0, w, h }, CUI_CORNERS(3), cui_color_of(ui, CUI_TRANSPARENT), cui_color_of(ui, CUI_COLOR_FOREGROUND), 1.0f);
    cui_draw_rect(ui, (cui_rect){ x0, y0, w * 0.3f, h }, CUI_CORNERS(3), col);
}

/* 画"上下分隔"图标 */
static void split_fm_icon_paint(cui_ctx *ui, cui_node *n, void *data) {
    (void)data;
    cui_rect r = n->rect;
    /* 内边距 8 */
    float x0 = r.x + 8, y0 = r.y + 8;
    float w = r.w - 16, h = r.h - 16;
    cui_color col = cui_alpha(CUI_C(ui, FOREGROUND), 0.9f);
    /* 左右竖线 */
    cui_draw_rect_border(ui, (cui_rect){ x0, y0, w, h }, CUI_CORNERS(3), cui_color_of(ui, CUI_TRANSPARENT), cui_color_of(ui, CUI_COLOR_FOREGROUND), 1.0f);
    cui_draw_rect(ui, (cui_rect){ x0, y0 + h * 0.7f - 0.7f, w, h * 0.3f }, CUI_CORNERS(0), col);
}

/* ================================================================== */
/* frame                                                               */
/* ================================================================== */

static void frame(cui_ctx *ui, void *user) {
    (void)user;
    check_uploads(ui);
    check_reconnects(ui);
    check_download(ui);
    check_upload(ui);
    poll_forwards(ui); 
    /* ← 新增：每帧请求唤醒，保证转发持续处理 */
    {
        bool any_fwd = false;
        for (int i = 0; i < g_forward_count; i++)
            if (g_forwards[i].running) { any_fwd = true; break; }
        if (any_fwd) cui_request_wakeup(ui, 0.01f);
    }

    float now = (float)cui_time(ui);
    if (now - g_last_refresh >= 5.0f) {
        g_last_refresh = now;
        if (g_tab_count > 0 && g_tab_ssh[g_ssh_tab]) refresh_stats_real(0);
        else refresh_stats();
        cui_request_wakeup(ui, 5.0f);
    }

    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 0) {
        cui_column(ui, .width = CUI_FILL, .height = CUI_GROW(1), .gap = 0) {
            cui_row(ui, .width = CUI_FILL, .height = CUI_PX(44), .pad_x = 12,
                    .align = CUI_ALIGN_CENTER, .bg = CUI_COLOR_SURFACE_MUTED) {
                cui_spacer(ui);
                /* 左侧栏显示/隐藏 */
                // if (cui_icon_button(ui, g_show_sidebar ? CUI_ICON_PANEL_LEFT : CUI_ICON_PANEL_LEFT,
                //                     .tooltip = g_show_sidebar ? T("隐藏左侧栏", "Hide sidebar") : T("显示左侧栏", "Show sidebar"),
                //                     .selected = !g_show_sidebar)) {
                //     g_show_sidebar = !g_show_sidebar;
                // }
                {
                    cui_id lp_btn_id = cui_make_id(ui, "toggle-lp-panel");
                    int lp_btn_node = cui__node_new(ui, lp_btn_id);
                    ui->nodes[lp_btn_node].w = CUI_PX(30);
                    ui->nodes[lp_btn_node].h = CUI_PX(30);
                    ui->nodes[lp_btn_node].radius = CUI_CORNERS(6);
                    ui->nodes[lp_btn_node].bg = g_show_sidebar
                                             ? cui_color_of(ui, CUI_COLOR_HOVER)
                                             : CUI_TRANSPARENT;
                    ui->nodes[lp_btn_node].paint = split_lp_icon_paint;
                    ui->nodes[lp_btn_node].paint_data = NULL;
                    cui__mark_interactive(ui, lp_btn_node, 0);
                    cui_interact vr_lp = cui__interact(ui, lp_btn_id, 0);
                    static bool prev_pressed_lp = false;
                    bool lp_clicked_now = vr_lp.pressed && !prev_pressed_lp;
                    prev_pressed_lp = vr_lp.pressed;
                    if (lp_clicked_now) g_show_sidebar = !g_show_sidebar;
                    if (vr_lp.hovered) cui__set_cursor(ui, CUI_CURSOR_HAND);
                }
                /* 文件/命令面板显示/隐藏 */
                // if (cui_icon_button(ui, CUI_ICON_MENU,
                //                     .tooltip = g_show_fm_panel ? T("隐藏文件面板", "Hide file panel") : T("显示文件面板", "Show file panel"),
                //                     .selected = !g_show_fm_panel)) {
                //     g_show_fm_panel = !g_show_fm_panel;
                // }
                /* 文件/命令面板显示/隐藏 */
                {
                    cui_id btn_id = cui_make_id(ui, "toggle-fm-panel");
                    int btn_node = cui__node_new(ui, btn_id);
                    ui->nodes[btn_node].w = CUI_PX(30);
                    ui->nodes[btn_node].h = CUI_PX(30);
                    ui->nodes[btn_node].radius = CUI_CORNERS(6);
                    ui->nodes[btn_node].bg = g_show_fm_panel
                                             ? cui_color_of(ui, CUI_COLOR_HOVER)
                                             : CUI_TRANSPARENT;
                    ui->nodes[btn_node].paint = split_fm_icon_paint;
                    ui->nodes[btn_node].paint_data = NULL;
                    cui__mark_interactive(ui, btn_node, 0);
                    cui_interact vr = cui__interact(ui, btn_id, 0);
                    static bool prev_pressed_fm = false;
                    bool clicked_now = vr.pressed && !prev_pressed_fm;
                    prev_pressed_fm = vr.pressed;
                    if (clicked_now) g_show_fm_panel = !g_show_fm_panel;
                    if (vr.hovered) cui__set_cursor(ui, CUI_CURSOR_HAND);
                }
                /* 明暗切换 */
                if (cui_icon_button(ui, g_dark ? CUI_ICON_SUN : CUI_ICON_MOON,
                                    .tooltip = g_dark ? T("浅色", "Light") : T("深色", "Dark"))) {
                    g_dark = !g_dark;
                    cui_set_theme(ui, g_dark ? CUI_THEME_DARK : CUI_THEME_LIGHT);
                }
                /* 语言切换 */
                if (cui_icon_button(ui, CUI_ICON_GLOBE, .tooltip = (lang == 'z') ? "EN" : "中文"))
                    lang = (lang == 'z') ? 'e' : 'z';
                /* 设置 */
                if (cui_icon_button(ui, CUI_ICON_SETTINGS, .tooltip = T("设置", "Settings")))
                    g_settings_dlg = true;
                /* 关于 */
                if (cui_icon_button(ui, CUI_ICON_INFO, .tooltip = T("关于", "About"))) {
                    g_about_dlg = true;
                }
            }
            hline(ui);

            cui_split_opts so = { ._ = 0, .vertical = false, .min_first = 180, .min_second = 400 };
            cui_split_begin(ui, "main-split", &g_split_ratio, &so);
            if (g_show_sidebar) {
                cui_split_opts so = { ._ = 0, .vertical = false, .min_first = 180, .min_second = 400 };
                cui_split_begin(ui, "main-split", &g_split_ratio, &so);
                cui_split_opts lvso = { ._ = 0, .vertical = true, .min_first = 120, .min_second = 200 };
                cui_split_begin(ui, "left-split", &g_left_v_split, &lvso);
                cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 6) { sessions_panel(ui); }
                cui_split_next(ui);
                cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 6) {
                    cui_scroll(ui, .id = "left-stats-scroll", .height = CUI_FILL) {
                        cui_column(ui, .width = CUI_FILL, .gap = 8) {
                            stats_panel(ui);
                            hline(ui);
                            dirs_panel(ui);
                        }
                    }
                }
                cui_split_end(ui);
                cui_split_next(ui);
                cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 10, .gap = 8) {
                    right_panel(ui);
                }
                cui_split_end(ui);
            } else {
                cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 10, .gap = 8) {
                    right_panel(ui);
                }
            }
            

            // cui_split_next(ui);
            // cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 10, .gap = 8) {
            //     cui_split_opts vso = { ._ = 0, .vertical = true, .min_first = 200, .min_second = 160 };
            //     cui_split_begin(ui, "right-split", &g_v_split, &vso);
            //     terminals_panel(ui);
            //     if(g_show_fm_panel){
            //         cui_split_next(ui);
            //         cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 4) {
            //             static const char *lz[] = { "文件", "命令" };
            //             static const char *le[] = { "Files", "Commands" };
            //             cui_tabs_ex(ui, "fm-tabs", lang == 'z' ? lz : le, 2, &g_fm_tab, NULL);
            //             if (g_fm_tab == 0) files_panel(ui);
            //             else                commands_panel(ui);
            //         }
            //     }
                        //    
            //     cui_split_end(ui);
            // }
            cui_split_end(ui);
        }
        // hline(ui);
        // if (cui_button(ui, g_status[0] ? g_status : T("就绪", "Ready"),
        //                .variant = CUI_BUTTON_GHOST, .full_width = true, .size = CUI_SIZE_SM))
        //     g_log_dlg = true;
        hline(ui);
        cui_id status_id = cui_make_id(ui, "statusbar");
        int status_node = cui__node_new(ui, status_id);
        ui->nodes[status_node].w = CUI_FILL;
        ui->nodes[status_node].h = CUI_PX(26);
        ui->nodes[status_node].bg = cui_color_of(ui, CUI_COLOR_SURFACE_MUTED);
        ui->nodes[status_node].pad[1] = 10;
        ui->nodes[status_node].pad[3] = 10;
        cui__open(ui, status_node);
            cui_row(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_CENTER, .gap = 12) {
                /* 左侧：状态文本，点击打开日志 */
                cui_box(ui, .width = CUI_GROW(1)) {
                    cui_text(ui, g_status[0] ? g_status : T("就绪", "Ready"),
                             .size = 11, .color = CUI_COLOR_TEXT_SECONDARY,
                             .nowrap = true);
                }
                /* 右侧：固定提示 */
                cui_text(ui, T("Ctrl+Shift+V 粘贴", "Ctrl+Shift+V paste"),
                         .size = 11, .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
            }
        cui__close(ui);
        cui__mark_interactive(ui, status_node, 0);
        cui_interact status_vr = cui__interact(ui, status_id, 0);
        if (status_vr.pressed) {
            g_log_dlg = true;
        }
    }

    new_edit_dialog(ui);
    delete_dialog(ui);
    retry_dialog(ui);
    log_dialog(ui);
    settings_dialog(ui);
    nav_error_dialog(ui); 
    transfers_dialog(ui); 
    newfile_dialog(ui);
    newdir_dialog(ui);
    props_dialog(ui);
    dir_props_dialog(ui);
    paste_dialog(ui);
    file_ctx_dialog(ui);
    delete_file_dialog(ui);
    about_dialog(ui);
    history_dialog(ui);
    cmd_add_dialog(ui);
    linux_run_dialog(ui);
    linux_edit_dialog(ui);
    linux_del_dialog(ui);
    rename_dialog(ui);
    forwards_dialog(ui);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    ssh_init();
    load_sessions();
    load_transfers(); 
    load_cmds();
    load_linux_cmds();

    cui_app *app = cui_app_create(&(cui_app_desc){
        .title = "SSH Terminal",
        .width = g_win_w, .height = g_win_h,      /* ← 用读到的 */
        .accent = CUI_ACCENT_BLUE,
        .theme = g_dark ? CUI_THEME_DARK : CUI_THEME_LIGHT,   /* ← 新增 */
    });
    if (!app) { ssh_finalize(); return 1; }
#ifdef _WIN32
{
    HWND hwnd = (HWND)cui_app_native_window(app);
    if (hwnd) {
        char icon_path[MAX_PATH];
        GetModuleFileNameA(NULL, icon_path, MAX_PATH);
        char *p = strrchr(icon_path, '\\');
        if (p) strcpy(p + 1, "ssh_terminal.ico");
        HICON hIcon = (HICON)LoadImageA(NULL, icon_path, IMAGE_ICON,
                                        0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        if (hIcon) {
            SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
        }
    }
}
#endif
    
    cui_ctx *ui = cui_app_ui(app);

    const char *paths[] = {
        "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/simsun.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/System/Library/Fonts/PingFang.ttc",
    };
    cui_font zh = -1;
    for (int i = 0; i < (int)(sizeof paths/sizeof paths[0]) && zh < 0; i++)
        zh = cui_font_load_file(ui, paths[i]);
    if (zh >= 0) {
        cui_font_add_fallback(ui, CUI_FONT_BODY, zh);
        cui_font_add_fallback(ui, CUI_FONT_MEDIUM, zh);
        cui_font_add_fallback(ui, CUI_FONT_MONO, zh);
        cui_font_add_fallback(ui, CUI_FONT_DISPLAY, zh);
        cui_font_add_fallback(ui, CUI_FONT_ICONS, zh);
    }

    while (cui_app_frame(app)) frame(ui, NULL);
    /* 退出前保存窗口大小 + 所有设置 */
    {
        cui_vec2 vp = cui_viewport_size(ui);
        if (vp.x > 0 && vp.y > 0) {
            g_win_w = (int)(vp.x + 0.5f);
            g_win_h = (int)(vp.y + 0.5f);
        }
        save_sessions();
        save_linux_cmds();
        linux_cmds_free();
    }
    for (int k = 0; k < g_tab_count; k++) {
        if (g_tab_sftp[k]) { sftp_free(g_tab_sftp[k]); g_tab_sftp[k] = NULL; }
        if (g_tab_chan[k]) {
            ssh_channel_send_eof(g_tab_chan[k]);
            ssh_channel_close(g_tab_chan[k]);
            ssh_channel_free(g_tab_chan[k]);
            g_tab_chan[k] = NULL;
        }
        if (g_tab_ssh[k]) {
            ssh_disconnect(g_tab_ssh[k]);
            ssh_free(g_tab_ssh[k]);
            g_tab_ssh[k] = NULL;
        }
    }
    for (int k = 0; k < PROC_COUNT; k++) {
        if (g_procs[k].name) { free(g_procs[k].name); g_procs[k].name = NULL; }
    }
    for (int i = 0; i < g_forward_count; i++)
        if (g_forwards[i].running) stop_forward(i);
    cui_app_destroy(app);
    ssh_finalize();
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}