
#include <errno.h>

#include "sigar.h"
#include "sigar_private.h"
#include "sigar_util.h"
#include "sigar_os.h"

#ifndef WIN32
#include <signal.h>
#endif

SIGAR_DECLARE(int) sigar_open(sigar_t **sigar)
{
    int status = sigar_os_open(sigar);

    if (status == SIGAR_OK) {
        (*sigar)->pid = 0;
        (*sigar)->ifconf_buf = NULL;
        (*sigar)->ifconf_len = 0;
        (*sigar)->log_level = -1; /* log nothing by default */
        (*sigar)->log_impl = NULL;
        (*sigar)->log_data = NULL;
    }

    return status;
}

SIGAR_DECLARE(int) sigar_close(sigar_t *sigar)
{
    if (sigar->ifconf_buf) {
        free(sigar->ifconf_buf);
    }
    
    return sigar_os_close(sigar);
}

#ifndef __linux__ /* linux has a special case */
SIGAR_DECLARE(sigar_pid_t) sigar_pid_get(sigar_t *sigar)
{
    if (!sigar->pid) {
        sigar->pid = getpid();
    }

    return sigar->pid;
}
#endif

SIGAR_DECLARE(int) sigar_proc_kill(sigar_pid_t pid, int signum)
{
#ifdef WIN32
    int status = -1;
    HANDLE proc =
        OpenProcess(PROCESS_ALL_ACCESS,
                    TRUE, (DWORD)pid);

    if (proc) {
        switch (signum) {
          case 0:
            status = SIGAR_OK;
            break;
          default:
            if (TerminateProcess(proc, signum)) {
                status = SIGAR_OK;
            }
            break;
        }

        CloseHandle(proc);

        if (status == SIGAR_OK) {
            return SIGAR_OK;
        }
    }
    return GetLastError();
#else
    if (kill(pid, signum) == -1) {
        return errno;
    }
    return SIGAR_OK;
#endif
}

static char *sigar_error_string(int err)
{
    switch (err) {
      case SIGAR_ENOTIMPL:
        return "This function has not been implemented on this platform";
      default:
        return "Error string not specified yet";
    }
}

SIGAR_DECLARE(char *) sigar_strerror(sigar_t *sigar, int err)
{
    char *buf = NULL;
#ifdef WIN32
    DWORD len;
#endif

    if (err > SIGAR_OS_START_ERROR) {
        if ((buf = sigar_os_error_string(sigar, err)) != NULL) {
            return buf;
        }
        return "Unknown OS Error"; /* should never happen */
    }

    if (err > SIGAR_START_ERROR) {
        return sigar_error_string(err);
    }

#ifdef WIN32
    len = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS,
                        NULL,
                        err,
                        0, /* default language */
                        (LPTSTR)sigar->errbuf,
                        (DWORD)sizeof(sigar->errbuf),
                        NULL);
#else

#if defined(HAVE_STRERROR_R) && defined(HAVE_STRERROR_R_GLIBC)
    /*
     * strerror_r man page says:
     * "The GNU version may, but need not, use the user supplied buffer"
     */
    buf = strerror_r(err, sigar->errbuf, sizeof(sigar->errbuf));
#elif defined(HAVE_STRERROR_R)
    if (strerror_r(err, sigar->errbuf, sizeof(sigar->errbuf)) < 0) {
        buf = "Unknown Error";
    }
#else
    /* strerror() is thread safe on solaris and hpux */
    buf = strerror(err);
#endif

    if (buf != NULL) {
        SIGAR_SSTRCPY(sigar->errbuf, buf);
    }
    
#endif
    return sigar->errbuf;
}

#include <stdio.h> /* for sprintf */

SIGAR_DECLARE(int) sigar_uptime_string(sigar_t *sigar, 
                                       sigar_uptime_t *uptime,
                                       char *buffer,
                                       int buflen)
{
    char *ptr = buffer;
    int minutes, hours, days, offset = 0;

    /* XXX: get rid of sprintf and/or check for overflow */
    days = uptime->uptime / (60*60*24);

    if (days) {
        offset += sprintf(ptr + offset, "%d day%s, ",
                          days, (days > 1) ? "s" : "");
    }

    minutes = (int)uptime->uptime / 60;
    hours = minutes / 60;
    hours = hours % 24;
    minutes = minutes % 60;

    if (hours) {
        offset += sprintf(ptr + offset, "%2d:%02d",
                          hours, minutes);
    }
    else {
        offset += sprintf(ptr + offset, "%d min", minutes);
    }

    return SIGAR_OK;
}

/* copy apr_strfsize */
SIGAR_DECLARE(char *) sigar_format_size(sigar_uint64_t size, char *buf)
{
    const char ord[] = "KMGTPE";
    const char *o = ord;
    int remain;

    if (size == SIGAR_FIELD_NOTIMPL) {
        buf[0] = '-';
        buf[1] = '\0';
        return buf;
    }

    if (size < 973) {
        sprintf(buf, "%3d ", (int) size);
        return buf;
    }

    do {
        remain = (int)(size & 1023);
        size >>= 10;

        if (size >= 973) {
            ++o;
            continue;
        }

        if (size < 9 || (size == 9 && remain < 973)) {
            if ((remain = ((remain * 5) + 256) / 512) >= 10) {
                ++size;
                remain = 0;
            }
            sprintf(buf, "%d.%d%c", (int) size, remain, *o);
            return buf;
        }

        if (remain >= 512) {
            ++size;
        }

        sprintf(buf, "%3d%c", (int) size, *o);

        return buf;
    } while (1);
}

#ifndef WIN32
#include <pwd.h>
#include <grp.h>

int sigar_user_name_get(sigar_t *sigar, int uid, char *buf, int buflen)
{
    struct passwd *pw;
    /* XXX cache lookup */

# ifdef HAVE_GETPWUID_R
    struct passwd pwbuf;
    char buffer[512];

    if (getpwuid_r(uid, &pwbuf, buffer, sizeof(buffer), &pw) != 0) {
        return errno;
    }
# else
    if ((pw = getpwuid(uid)) == NULL) {
        return errno;
    }
# endif

    strncpy(buf, pw->pw_name, buflen);
    buf[buflen-1] = '\0';

    return SIGAR_OK;
}

int sigar_group_name_get(sigar_t *sigar, int gid, char *buf, int buflen)
{
    struct group *gr;
    /* XXX cache lookup */

# ifdef HAVE_GETGRGID_R
    struct group grbuf;
    char buffer[512];

    if (getgrgid_r(gid, &grbuf, buffer, sizeof(buffer), &gr) != 0) {
        return errno;
    }
# else
    if ((gr = getgrgid(gid)) == NULL) {
        return errno;
    }
# endif

    if (gr && gr->gr_name) {
        strncpy(buf, gr->gr_name, buflen);
    }
    else {
        /* seen on linux.. apache httpd.conf has:
         * Group #-1
         * results in uid == -1 and gr == NULL.
         * wtf getgrgid_r doesnt fail instead? 
         */
        sprintf(buf, "%d", gid);
    }
    buf[buflen-1] = '\0';

    return SIGAR_OK;
}

int sigar_user_id_get(sigar_t *sigar, const char *name, int *uid)
{
    /* XXX cache lookup */
    struct passwd *pw;

# ifdef HAVE_GETPWNAM_R
    struct passwd pwbuf;
    char buf[512];

    if (getpwnam_r(name, &pwbuf, buf, sizeof(buf), &pw) != 0) {
        return errno;
    }
# else
    if (!(pw = getpwnam(name))) {
        return errno;
    }
# endif

    *uid = (int)pw->pw_uid;
    return SIGAR_OK;
}

SIGAR_DECLARE(int)
sigar_proc_cred_name_get(sigar_t *sigar, sigar_pid_t pid,
                         sigar_proc_cred_name_t *proccredname)
{
    sigar_proc_cred_t cred;

    int status = sigar_proc_cred_get(sigar, pid, &cred);

    if (status != SIGAR_OK) {
        return status;
    }

    status = sigar_user_name_get(sigar, cred.uid,
                                 proccredname->user,
                                 sizeof(proccredname->user));

    if (status != SIGAR_OK) {
        return status;
    }

    status = sigar_group_name_get(sigar, cred.gid,
                                  proccredname->group,
                                  sizeof(proccredname->group));

    return status;
}

#endif /* WIN32 */

int sigar_proc_list_create(sigar_proc_list_t *proclist)
{
    proclist->number = 0;
    proclist->size = SIGAR_PROC_LIST_MAX;
    proclist->data = malloc(sizeof(*(proclist->data)) *
                            proclist->size);
    return SIGAR_OK;
}

int sigar_proc_list_grow(sigar_proc_list_t *proclist)
{
    proclist->data = realloc(proclist->data,
                             sizeof(*(proclist->data)) *
                             (proclist->size + SIGAR_PROC_LIST_MAX));
    proclist->size += SIGAR_PROC_LIST_MAX;

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_proc_list_destroy(sigar_t *sigar,
                                           sigar_proc_list_t *proclist)
{
    if (proclist->size) {
        free(proclist->data);
        proclist->number = proclist->size = 0;
    }

    return SIGAR_OK;
}

int sigar_proc_args_create(sigar_proc_args_t *procargs)
{
    procargs->number = 0;
    procargs->size = SIGAR_PROC_ARGS_MAX;
    procargs->data = malloc(sizeof(*(procargs->data)) *
                            procargs->size);
    return SIGAR_OK;
}

int sigar_proc_args_grow(sigar_proc_args_t *procargs)
{
    procargs->data = realloc(procargs->data,
                             sizeof(*(procargs->data)) *
                             (procargs->size + SIGAR_PROC_ARGS_MAX));
    procargs->size += SIGAR_PROC_ARGS_MAX;

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_proc_args_destroy(sigar_t *sigar,
                                           sigar_proc_args_t *procargs)
{
    unsigned int i;

    if (procargs->size) {
        for (i=0; i<procargs->number; i++) {
            free(procargs->data[i]);
        }
        free(procargs->data);
        procargs->number = procargs->size = 0;
    }

    return SIGAR_OK;
}

int sigar_file_system_list_create(sigar_file_system_list_t *fslist)
{
    fslist->number = 0;
    fslist->size = SIGAR_FS_MAX;
    fslist->data = malloc(sizeof(*(fslist->data)) *
                          fslist->size);
    return SIGAR_OK;
}

int sigar_file_system_list_grow(sigar_file_system_list_t *fslist)
{
    fslist->data = realloc(fslist->data,
                           sizeof(*(fslist->data)) *
                           (fslist->size + SIGAR_FS_MAX));
    fslist->size += SIGAR_FS_MAX;

    return SIGAR_OK;
}

/* indexed with sigar_file_system_type_e */
static const char *fstype_names[] = {
    "unknown", "none", "local", "remote", "ram", "cdrom", "swap"
};

static int sigar_common_fs_type_get(sigar_file_system_t *fsp)
{
    char *type = fsp->sys_type_name;

    switch (*type) {
      case 'n':
        if (strEQ(type, "nfs")) {
            fsp->type = SIGAR_FSTYPE_NETWORK;
        }
        break;
      case 's':
        if (strEQ(type, "smbfs")) { /* samba */
            fsp->type = SIGAR_FSTYPE_NETWORK;
        }
        else if (strEQ(type, "swap")) {
            fsp->type = SIGAR_FSTYPE_SWAP;
        }
        break;
      case 'a':
        if (strEQ(type, "afs")) {
            fsp->type = SIGAR_FSTYPE_NETWORK;
        }
        break;
      case 'i':
        if (strEQ(type, "iso9660")) {
            fsp->type = SIGAR_FSTYPE_CDROM;
        }
        break;
      case 'm':
        if (strEQ(type, "msdos") || strEQ(type, "minix")) {
            fsp->type = SIGAR_FSTYPE_LOCAL_DISK;
        }
        break;
      case 'h':
        if (strEQ(type, "hpfs")) {
            fsp->type = SIGAR_FSTYPE_LOCAL_DISK;
        }
        break;
      case 'v':
        if (strEQ(type, "vfat")) {
            fsp->type = SIGAR_FSTYPE_LOCAL_DISK;
        }
        break;
    }

    return fsp->type;
}

void sigar_fs_type_get(sigar_file_system_t *fsp)
{
    if (!(fsp->type ||                    /* already set */
          sigar_os_fs_type_get(fsp) ||    /* try os specifics first */
          sigar_common_fs_type_get(fsp))) /* try common ones last */
    {
        fsp->type = SIGAR_FSTYPE_NONE;
    }

    if (fsp->type >= SIGAR_FSTYPE_MAX) {
        fsp->type = SIGAR_FSTYPE_NONE;
    }

    strcpy(fsp->type_name, fstype_names[fsp->type]);
}


SIGAR_DECLARE(int)
sigar_file_system_list_destroy(sigar_t *sigar,
                               sigar_file_system_list_t *fslist)
{
    if (fslist->size) {
        free(fslist->data);
        fslist->number = fslist->size = 0;
    }

    return SIGAR_OK;
}

int sigar_cpu_info_list_create(sigar_cpu_info_list_t *cpu_infos)
{
    cpu_infos->number = 0;
    cpu_infos->size = SIGAR_CPU_INFO_MAX;
    cpu_infos->data = malloc(sizeof(*(cpu_infos->data)) *
                             cpu_infos->size);
    return SIGAR_OK;
}

int sigar_cpu_info_list_grow(sigar_cpu_info_list_t *cpu_infos)
{
    cpu_infos->data = realloc(cpu_infos->data,
                              sizeof(*(cpu_infos->data)) *
                              (cpu_infos->size + SIGAR_CPU_INFO_MAX));
    cpu_infos->size += SIGAR_CPU_INFO_MAX;

    return SIGAR_OK;
}

SIGAR_DECLARE(int)
sigar_cpu_info_list_destroy(sigar_t *sigar,
                            sigar_cpu_info_list_t *cpu_infos)
{
    if (cpu_infos->size) {
        free(cpu_infos->data);
        cpu_infos->number = cpu_infos->size = 0;
    }

    return SIGAR_OK;
}

int sigar_cpu_list_create(sigar_cpu_list_t *cpulist)
{
    cpulist->number = 0;
    cpulist->size = SIGAR_CPU_INFO_MAX;
    cpulist->data = malloc(sizeof(*(cpulist->data)) *
                           cpulist->size);
    return SIGAR_OK;
}

int sigar_cpu_list_grow(sigar_cpu_list_t *cpulist)
{
    cpulist->data = realloc(cpulist->data,
                            sizeof(*(cpulist->data)) *
                            (cpulist->size + SIGAR_CPU_INFO_MAX));
    cpulist->size += SIGAR_CPU_INFO_MAX;

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_cpu_list_destroy(sigar_t *sigar,
                                          sigar_cpu_list_t *cpulist)
{
    if (cpulist->size) {
        free(cpulist->data);
        cpulist->number = cpulist->size = 0;
    }

    return SIGAR_OK;
}

int sigar_net_route_list_create(sigar_net_route_list_t *routelist)
{
    routelist->number = 0;
    routelist->size = SIGAR_NET_ROUTE_LIST_MAX;
    routelist->data = malloc(sizeof(*(routelist->data)) *
                             routelist->size);
    return SIGAR_OK;
}

int sigar_net_route_list_grow(sigar_net_route_list_t *routelist)
{
    routelist->data =
        realloc(routelist->data,
                sizeof(*(routelist->data)) *
                (routelist->size + SIGAR_NET_ROUTE_LIST_MAX));
    routelist->size += SIGAR_NET_ROUTE_LIST_MAX;

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_net_route_list_destroy(sigar_t *sigar,
                                                sigar_net_route_list_t *routelist)
{
    if (routelist->size) {
        free(routelist->data);
        routelist->number = routelist->size = 0;
    }

    return SIGAR_OK;
}

int sigar_net_interface_list_create(sigar_net_interface_list_t *iflist)
{
    iflist->number = 0;
    iflist->size = SIGAR_NET_IFLIST_MAX;
    iflist->data = malloc(sizeof(*(iflist->data)) *
                          iflist->size);
    return SIGAR_OK;
}

int sigar_net_interface_list_grow(sigar_net_interface_list_t *iflist)
{
    iflist->data = realloc(iflist->data,
                           sizeof(*(iflist->data)) *
                           (iflist->size + SIGAR_NET_IFLIST_MAX));
    iflist->size += SIGAR_NET_IFLIST_MAX;

    return SIGAR_OK;
}

SIGAR_DECLARE(int)
sigar_net_interface_list_destroy(sigar_t *sigar,
                                 sigar_net_interface_list_t *iflist)
{
    unsigned int i;

    if (iflist->size) {
        for (i=0; i<iflist->number; i++) {
            free(iflist->data[i]);
        }
        free(iflist->data);
        iflist->number = iflist->size = 0;
    }

    return SIGAR_OK;
}

int sigar_net_connection_list_create(sigar_net_connection_list_t *connlist)
{
    connlist->number = 0;
    connlist->size = SIGAR_NET_CONNLIST_MAX;
    connlist->data = malloc(sizeof(*(connlist->data)) *
                            connlist->size);
    return SIGAR_OK;
}

int sigar_net_connection_list_grow(sigar_net_connection_list_t *connlist)
{
    connlist->data =
        realloc(connlist->data,
                sizeof(*(connlist->data)) *
                (connlist->size + SIGAR_NET_CONNLIST_MAX));
    connlist->size += SIGAR_NET_CONNLIST_MAX;

    return SIGAR_OK;
}

SIGAR_DECLARE(int)
sigar_net_connection_list_destroy(sigar_t *sigar,
                                  sigar_net_connection_list_t *connlist)
{
    if (connlist->size) {
        free(connlist->data);
        connlist->number = connlist->size = 0;
    }

    return SIGAR_OK;
}

SIGAR_DECLARE(const char *)sigar_net_connection_type_get(int type)
{
    switch (type) {
      case SIGAR_NETCONN_TCP:
        return "tcp";
      case SIGAR_NETCONN_UDP:
        return "udp";
      case SIGAR_NETCONN_RAW:
        return "raw";
      case SIGAR_NETCONN_UNIX:
        return "unix";
      default:
        return "unknown";
    }
}

SIGAR_DECLARE(const char *)sigar_net_connection_state_get(int state)
{
    switch (state) {
      case SIGAR_TCP_ESTABLISHED:
        return "ESTABLISHED";
      case SIGAR_TCP_SYN_SENT:
        return "SYN_SENT";
      case SIGAR_TCP_SYN_RECV:
        return "SYN_RECV";
      case SIGAR_TCP_FIN_WAIT1:
        return "FIN_WAIT1";
      case SIGAR_TCP_FIN_WAIT2:
        return "FIN_WAIT2";
      case SIGAR_TCP_TIME_WAIT:
        return "TIME_WAIT";
      case SIGAR_TCP_CLOSE:
        return "CLOSE";
      case SIGAR_TCP_CLOSE_WAIT:
        return "CLOSE_WAIT";
      case SIGAR_TCP_LAST_ACK:
        return "LAST_ACK";
      case SIGAR_TCP_LISTEN:
        return "LISTEN";
      case SIGAR_TCP_CLOSING:
        return "CLOSING";
      case SIGAR_TCP_IDLE:
        return "IDLE";
      case SIGAR_TCP_BOUND:
        return "BOUND";
      case SIGAR_TCP_UNKNOWN:
      default:
        return "UNKNOWN";
    }
}

int sigar_who_list_create(sigar_who_list_t *wholist)
{
    wholist->number = 0;
    wholist->size = SIGAR_WHO_LIST_MAX;
    wholist->data = malloc(sizeof(*(wholist->data)) *
                           wholist->size);
    return SIGAR_OK;
}

int sigar_who_list_grow(sigar_who_list_t *wholist)
{
    wholist->data = realloc(wholist->data,
                            sizeof(*(wholist->data)) *
                            (wholist->size + SIGAR_WHO_LIST_MAX));
    wholist->size += SIGAR_WHO_LIST_MAX;

    return SIGAR_OK;
}

SIGAR_DECLARE(int) sigar_who_list_destroy(sigar_t *sigar,
                                          sigar_who_list_t *wholist)
{
    if (wholist->size) {
        free(wholist->data);
        wholist->number = wholist->size = 0;
    }

    return SIGAR_OK;
}

#ifdef WIN32
SIGAR_DECLARE(int) sigar_who_list_get(sigar_t *sigar,
                                      sigar_who_list_t *wholist)
{
    return SIGAR_ENOTIMPL;
}

SIGAR_DECLARE(int) sigar_resource_limit_get(sigar_t *sigar,
                                            sigar_resource_limit_t *rlimit)
{
    MEMORY_BASIC_INFORMATION meminfo;
    memset(rlimit, -1, sizeof(*rlimit));

    if (VirtualQuery((LPCVOID)&meminfo, &meminfo, sizeof(meminfo))) {
        rlimit->stack_cur =
            (DWORD)&meminfo - (DWORD)meminfo.AllocationBase;
        rlimit->stack_max =
            ((DWORD)meminfo.BaseAddress + meminfo.RegionSize) -
            (DWORD)meminfo.AllocationBase;
    }

    rlimit->virtual_memory_max = rlimit->virtual_memory_cur =
        0x80000000UL;

    return SIGAR_OK;
}
#else

#ifdef __sun
#  include <utmpx.h>
#  define SIGAR_UTMP_FILE _UTMPX_FILE
#  define ut_time ut_tv.tv_sec
#else
#  include <utmp.h>
#  ifdef UTMP_FILE
#    define SIGAR_UTMP_FILE UTMP_FILE
#  else
#    define SIGAR_UTMP_FILE _PATH_UTMP
#  endif
#endif

#if defined(__FreeBSD__) || defined(DARWIN)
#  define ut_user ut_name
#endif

#define WHOCPY(dest, src) \
    SIGAR_SSTRCPY(dest, src); \
    if (sizeof(src) < sizeof(dest)) \
        dest[sizeof(src)] = '\0'

int sigar_who_list_get(sigar_t *sigar,
                       sigar_who_list_t *wholist)
{
    FILE *fp;
#ifdef __sun
    struct utmpx ut;
#else
    struct utmp ut;
#endif
    if (!(fp = fopen(SIGAR_UTMP_FILE, "r"))) {
        return errno;
    }

    sigar_who_list_create(wholist);

    while (fread(&ut, sizeof(ut), 1, fp) == 1) {
        sigar_who_t *who;

        if (*ut.ut_name == '\0') {
            continue;
        }

#ifdef USER_PROCESS
        if (ut.ut_type != USER_PROCESS) {
            continue;
        }
#endif

        SIGAR_WHO_LIST_GROW(wholist);
        who = &wholist->data[wholist->number++];

        WHOCPY(who->user, ut.ut_user);
        WHOCPY(who->device, ut.ut_line);
        WHOCPY(who->host, ut.ut_host);

        who->time = ut.ut_time;
    }

    fclose(fp);

    return SIGAR_OK;
}

#include <sys/resource.h>

#define OffsetOf(structure, field) \
   (size_t)(&((structure *)NULL)->field)

#define RlimitOffsets(field) \
    OffsetOf(sigar_resource_limit_t, field##_cur), \
    OffsetOf(sigar_resource_limit_t, field##_max)

#define RlimitSet(structure, ptr, val) \
    *(sigar_uint64_t *)((char *)structure + (int)(long)ptr) = val

typedef struct {
    int resource;
    size_t cur;
    size_t max;
} rlimit_field_t;

#define RLIMIT_UNSUPPORTED (RLIM_NLIMITS+1)

#ifndef RLIMIT_RSS
#define RLIMIT_RSS RLIMIT_UNSUPPORTED
#endif

static rlimit_field_t sigar_rlimits[] = {
    { RLIMIT_CPU, RlimitOffsets(cpu) },
    { RLIMIT_FSIZE, RlimitOffsets(file_size) },
    { RLIMIT_DATA, RlimitOffsets(data) },
    { RLIMIT_STACK, RlimitOffsets(stack) },
    { RLIMIT_CORE, RlimitOffsets(core) },
    { RLIMIT_RSS, RlimitOffsets(memory) },
    { RLIMIT_NPROC, RlimitOffsets(processes) },
    { RLIMIT_NOFILE, RlimitOffsets(open_files) },
    { RLIMIT_AS, RlimitOffsets(virtual_memory) },
    { -1 }
};

int sigar_resource_limit_get(sigar_t *sigar,
                             sigar_resource_limit_t *rlimit)
{
    int i;

    rlimit->unlimited = RLIM_INFINITY;

    for (i=0; sigar_rlimits[i].resource != -1; i++) {
        struct rlimit rl;
        rlimit_field_t *r = &sigar_rlimits[i];

        if ((r->resource == RLIMIT_UNSUPPORTED) ||
            (getrlimit(r->resource, &rl) != 0))
        {
            rl.rlim_cur = SIGAR_FIELD_NOTIMPL;
            rl.rlim_max = SIGAR_FIELD_NOTIMPL;
        }

        RlimitSet(rlimit, r->cur, rl.rlim_cur);
        RlimitSet(rlimit, r->max, rl.rlim_max);
    }

    return SIGAR_OK;
}
#endif

void sigar_hwaddr_format(char *buff, unsigned char *ptr)
{
    sprintf(buff, "%02X:%02X:%02X:%02X:%02X:%02X",
            (ptr[0] & 0377), (ptr[1] & 0377), (ptr[2] & 0377),
            (ptr[3] & 0377), (ptr[4] & 0377), (ptr[5] & 0377));
}

#if !defined(WIN32) && !defined(DARWIN) && !defined(__FreeBSD__)

/* XXX: prolly will be moving these stuffs into os_net.c */
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <net/if.h>

#ifndef SIOCGIFCONF
#include <sys/sockio.h>
#endif

#if defined(_AIX) || defined(__osf__) /* good buddies */

#include <net/if_dl.h>

static void hwaddr_aix_lookup(sigar_t *sigar, sigar_net_interface_config_t *ifconfig)
{
    char *ent, *end;
    struct ifreq *ifr;

    /* XXX: assumes sigar_net_interface_list_get has been called */
    end = sigar->ifconf_buf + sigar->ifconf_len;

    for (ent = sigar->ifconf_buf;
         ent < end;
         ent += sizeof(*ifr))
    {
        ifr = (struct ifreq *)ent;

        if (ifr->ifr_addr.sa_family != AF_LINK) {
            continue;
        }

        if (strEQ(ifr->ifr_name, ifconfig->name)) {
            struct sockaddr_dl *sdl = (struct sockaddr_dl *)&ifr->ifr_addr;

            sigar_hwaddr_format(ifconfig->hwaddr,
                                (unsigned char *)LLADDR(sdl));
            return;
        }
    }

    sigar_hwaddr_set_null(ifconfig);
}

#elif !defined(SIOCGIFHWADDR)

#include <net/if_arp.h>

static void hwaddr_arp_lookup(sigar_net_interface_config_t *ifconfig, int sock)
{
    struct arpreq areq;
    struct sockaddr_in *sa;

    memset(&areq, 0, sizeof(areq));
    sa = (struct sockaddr_in *)&areq.arp_pa;
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = ifconfig->address;
    
    if (ioctl(sock, SIOCGARP, &areq) < 0) {
        /* ho-hum */
        memset(&areq.arp_ha.sa_data, '\0', sizeof(areq.arp_ha.sa_data));
    }

    sigar_hwaddr_format(ifconfig->hwaddr,
                        (unsigned char *)areq.arp_ha.sa_data);
}

#endif

int sigar_net_interface_config_get(sigar_t *sigar, const char *name,
                                   sigar_net_interface_config_t *ifconfig)
{
    int sock;
    struct ifreq ifr;

    SIGAR_ZERO(ifconfig);

    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        return errno;
    }

    SIGAR_SSTRCPY(ifconfig->name, name);
    SIGAR_SSTRCPY(ifr.ifr_name, name);

#define ifr_s_addr(ifr) \
    ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr

    if (!ioctl(sock, SIOCGIFADDR, &ifr)) {
        ifconfig->address = ifr_s_addr(ifr);
    }
    else {
        /* if this one failed, so will everything else */
        close(sock);
        return errno;
    }

    if (!ioctl(sock, SIOCGIFNETMASK, &ifr)) {
        ifconfig->netmask = ifr_s_addr(ifr);
    }
    
    if (!ioctl(sock, SIOCGIFFLAGS, &ifr)) {
        ifconfig->flags = ifr.ifr_flags;
#ifdef __linux__
        /*
         * XXX: should just define SIGAR_IFF_*
         * and test IFF_* bits on given platform.
         * this is the only diff between solaris/hpux/linux
         * for the flags we care about.
         *
         */
        if (ifconfig->flags & IFF_MULTICAST) {
            ifconfig->flags |= SIGAR_IFF_MULTICAST;
        }
        else {
            /* 0x800 == IFF_SLAVE on linux */
            ifconfig->flags &= ~SIGAR_IFF_MULTICAST;
        }
#endif
    }
    else {
        /* should always be able to get flags for existing device */
        /* other ioctls may fail if device is not enabled: ok */
        close(sock);
        return errno;
    }

    if (ifconfig->flags & IFF_LOOPBACK) {
        ifconfig->destination = ifconfig->address;
        ifconfig->broadcast = 0;
        sigar_hwaddr_set_null(ifconfig);
    }
    else {
        if (!ioctl(sock, SIOCGIFDSTADDR, &ifr)) {
            ifconfig->destination = ifr_s_addr(ifr);
        }

        if (!ioctl(sock, SIOCGIFBRDADDR, &ifr)) {
            ifconfig->broadcast = ifr_s_addr(ifr);
        }

#if defined(SIOCGIFHWADDR)
        if (!ioctl(sock, SIOCGIFHWADDR, &ifr)) {
            sigar_hwaddr_format(ifconfig->hwaddr, ifr.ifr_hwaddr.sa_data);
        }
#elif defined(_AIX) || defined(__osf__)
        hwaddr_aix_lookup(sigar, ifconfig);
#else
        hwaddr_arp_lookup(ifconfig, sock);
#endif
    }

#ifdef __linux__    
    if (!ioctl(sock, SIOCGIFMTU, &ifr)) {
        ifconfig->mtu = ifr.ifr_mtu;
    }
#else
    ifconfig->mtu = 0; /*XXX*/
#endif
    
    if (!ioctl(sock, SIOCGIFMETRIC, &ifr)) {
        ifconfig->metric = ifr.ifr_metric ? ifr.ifr_metric : 1;
    }

    close(sock);    

    return SIGAR_OK;
}

#ifdef _AIX
#  define MY_SIOCGIFCONF CSIOCGIFCONF
#else
#  define MY_SIOCGIFCONF SIOCGIFCONF
#endif

#ifdef __osf__
static int sigar_netif_configured(sigar_t *sigar, char *name)
{
    int status;
    sigar_net_interface_config_t ifconfig;

    status = sigar_net_interface_config_get(sigar, name, &ifconfig);

    return status == SIGAR_OK;
}
#endif

int sigar_net_interface_list_get(sigar_t *sigar,
                                 sigar_net_interface_list_t *iflist)
{
    int n, lastlen=0;
    struct ifreq *ifr;
    struct ifconf ifc;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        return errno;
    } 

    for (;;) {
        if (!sigar->ifconf_buf || lastlen) {
            sigar->ifconf_len += sizeof(struct ifreq) * SIGAR_NET_IFLIST_MAX;
            sigar->ifconf_buf = realloc(sigar->ifconf_buf, sigar->ifconf_len);
        }

        ifc.ifc_len = sigar->ifconf_len;
        ifc.ifc_buf = sigar->ifconf_buf;

        if (ioctl(sock, MY_SIOCGIFCONF, &ifc) < 0) {
            /* EINVAL should mean num_interfaces > ifc.ifc_len */
            if ((errno != EINVAL) ||
                (lastlen == ifc.ifc_len))
            {
                free(ifc.ifc_buf);
                return errno;
            }
        }

        if (ifc.ifc_len < sigar->ifconf_len) {
            break; /* got em all */
        }

        if (ifc.ifc_len != lastlen) {
            /* might be more */
            lastlen = ifc.ifc_len;
            continue;
        }

        break;
    }

    close(sock);

    iflist->number = 0;
    iflist->size = ifc.ifc_len;
    iflist->data = malloc(sizeof(*(iflist->data)) *
                          iflist->size);

    ifr = ifc.ifc_req;
    for (n = 0; n < ifc.ifc_len; n += sizeof(struct ifreq), ifr++) {
#if defined(_AIX) || defined(__osf__) /* pass the bourbon */
        if (ifr->ifr_addr.sa_family != AF_LINK) {
            /* XXX: dunno if this is right.
             * otherwise end up with two 'en0' and three 'lo0'
             * with the same ip address.
             */
            continue;
        }
#   ifdef __osf__
        /* weed out "sl0", "tun0" and the like */
        /* XXX must be a better way to check this */
        if (!sigar_netif_configured(sigar, ifr->ifr_name)) {
            continue;
        }
#   endif        
#endif
        iflist->data[iflist->number++] = strdup(ifr->ifr_name);
    }

    return SIGAR_OK;
}

#endif /* WIN32 */

#ifndef WIN32
#include <netinet/in.h>
#endif

/* threadsafe alternative to inet_ntoa (inet_ntop4 from apr) */
SIGAR_DECLARE(int) sigar_inet_ntoa(sigar_t *sigar,
                                   sigar_uint64_t address,
                                   char *addr_str)
{
    char *next=addr_str;
    int n=0;
    const unsigned char *src;
    struct in_addr addr;

    addr.s_addr = address;

    src = (const unsigned char *)&addr.s_addr;

    do {
        unsigned char u = *src++;
        if (u > 99) {
            *next++ = '0' + u/100;
            u %= 100;
            *next++ = '0' + u/10;
            u %= 10;
        }
        else if (u > 9) {
            *next++ = '0' + u/10;
            u %= 10;
        }
        *next++ = '0' + u;
        *next++ = '.';
        n++;
    } while (n < 4);

    *--next = 0;

    return SIGAR_OK;
}

static int fqdn_ip_get(sigar_t *sigar, char *name)
{
    int i, status;
    sigar_net_interface_list_t iflist;

    if ((status = sigar_net_interface_list_get(sigar, &iflist)) != SIGAR_OK) {
        return status;
    }

    for (i=0; i<iflist.number; i++) {
        sigar_net_interface_config_t ifconfig;

        status = sigar_net_interface_config_get(sigar,
                                                iflist.data[i], &ifconfig);

        if ((status != SIGAR_OK) ||
            (ifconfig.flags & SIGAR_IFF_LOOPBACK))
        {
            continue;
        }

        sigar_inet_ntoa(sigar, ifconfig.address, name);

        sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                         "[fqdn] using ip address '%s' for fqdn",
                         name);

        break;
    }

    sigar_net_interface_list_destroy(sigar, &iflist);

    return SIGAR_OK;
}

#ifdef WIN32
#else
#include <netdb.h>
#endif

#define IS_FQDN(name) \
    strchr(name, '.')

#define H_ALIAS_MATCH(alias, name) \
    (IS_FQDN(alias) && strnEQ(alias, name, strlen(name)))

#define FQDN_SET(fqdn) \
    SIGAR_STRNCPY(name, fqdn, namelen)

SIGAR_DECLARE(int) sigar_fqdn_get(sigar_t *sigar, char *name, int namelen)
{
    struct hostent *p;
    char domain[SIGAR_FQDN_LEN + 1];
#ifdef WIN32
    int status = sigar_wsa_init(sigar);

    if (status != SIGAR_OK) {
        return status;
    }
#endif

    if (gethostname(name, namelen - 1) != 0) {
        sigar_log_printf(sigar, SIGAR_LOG_ERROR,
                         "[fqdn] gethostname failed: %s",
                         sigar_strerror(sigar, errno));
        return errno;
    }
    else {
        if (SIGAR_LOG_IS_DEBUG(sigar)) {
            sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                             "[fqdn] gethostname() returned: '%s'",
                             name);
        }
    }

    /* XXX use _r versions of these functions. */
    if (!(p = gethostbyname(name))) {
        if (SIGAR_LOG_IS_DEBUG(sigar)) {
            sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                             "[fqdn] gethostbyname(%s) failed: %s",
                             name, sigar_strerror(sigar, errno));
        }

        if (!IS_FQDN(name)) {
            fqdn_ip_get(sigar, name);
        }

        return SIGAR_OK;
    }

    if (IS_FQDN(p->h_name)) {
        FQDN_SET(p->h_name);

        sigar_log(sigar, SIGAR_LOG_DEBUG,
                  "[fqdn] resolved using gethostbyname.h_name");

        return SIGAR_OK;
    }
    else {
        sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                         "[fqdn] unresolved using gethostbyname.h_name");
    }

    if (p->h_aliases) {
        int i;

        for (i=0; p->h_aliases[i]; i++) {
            if (H_ALIAS_MATCH(p->h_aliases[i], p->h_name)) {
                FQDN_SET(p->h_aliases[i]);

                sigar_log(sigar, SIGAR_LOG_DEBUG,
                          "[fqdn] resolved using gethostbyname.h_aliases");

                return SIGAR_OK;
            }
        }
    }

    sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                     "[fqdn] unresolved using gethostbyname.h_aliases");

    if (p->h_addr_list) {
        int i,j;

        for (i=0; p->h_addr_list[i]; i++) {
            struct hostent *q = 
                gethostbyaddr(p->h_addr_list[i],
                              p->h_length,
                              p->h_addrtype);
            
            if (IS_FQDN(q->h_name)) {
                FQDN_SET(q->h_name);

                sigar_log(sigar, SIGAR_LOG_DEBUG,
                          "[fqdn] resolved using gethostbyaddr.h_name");

                return SIGAR_OK;
            }
            else {
                for (j=0; q->h_aliases[j]; j++) {
                    if (H_ALIAS_MATCH(q->h_aliases[j], q->h_name)) {
                        FQDN_SET(q->h_aliases[j]);

                        sigar_log(sigar, SIGAR_LOG_DEBUG,
                                  "[fqdn] resolved using "
                                  "gethostbyaddr.h_aliases");

                        return SIGAR_OK;
                    }
                }
            }
        }
    }

    sigar_log_printf(sigar, SIGAR_LOG_DEBUG,
                     "[fqdn] unresolved using gethostbyname.h_addr_list");

#ifndef WIN32
    if (!IS_FQDN(name) && /* e.g. aix gethostname is already fqdn */
        (getdomainname(domain, sizeof(domain) - 1) == 0) &&
        (domain[0] != '\0') &&
        (domain[0] != '('))  /* linux default is "(none)" */
    {
        /* sprintf(name, "%s.%s", name, domain); */
        char *ptr = name;
        int len = strlen(name);
        ptr += len;
        *ptr++ = '.';
        namelen -= (len+1);
        SIGAR_STRNCPY(ptr, domain, namelen);

        sigar_log(sigar, SIGAR_LOG_DEBUG,
                  "[fqdn] resolved using getdomainname");
    }
    else {
        sigar_log(sigar, SIGAR_LOG_DEBUG,
                  "[fqdn] getdomainname failed");
    }
#endif

    if (!IS_FQDN(name)) {
        fqdn_ip_get(sigar, name);
    }

    return SIGAR_OK;
}

#ifndef MAX_STRING_LEN
#define MAX_STRING_LEN 8192
#endif

#ifdef WIN32
/* The windows version of getPasswordNative was lifted from apr */
SIGAR_DECLARE(char *) sigar_password_get(const char *prompt)
{
    static char password[MAX_STRING_LEN];
    int n = 0;
    int ch;

    fputs(prompt, stderr);
    fflush(stderr);

    while ((ch = _getch()) != '\r') {
        if (ch == EOF) /* EOF */ {
            return NULL;
        }
        else if (ch == 0 || ch == 0xE0) {
            /* FN Keys (0 or E0) are a sentinal for a FN code */ 
            ch = (ch << 4) | _getch();
            /* Catch {DELETE}, {<--}, Num{DEL} and Num{<--} */
            if ((ch == 0xE53 || ch == 0xE4B || ch == 0x053 || ch == 0x04b) && n) {
                password[--n] = '\0';
                fputs("\b \b", stderr);
                fflush(stderr);
            }
            else {
                fputc('\a', stderr);
                fflush(stderr);
            }
        }
        else if ((ch == '\b' || ch == 127) && n) /* BS/DEL */ {
            password[--n] = '\0';
            fputs("\b \b", stderr);
            fflush(stderr);
        }
        else if (ch == 3) /* CTRL+C */ {
            /* _getch() bypasses Ctrl+C but not Ctrl+Break detection! */
            fputs("^C\n", stderr);
            fflush(stderr);
            exit(-1);
        }
        else if (ch == 26) /* CTRL+Z */ {
            fputs("^Z\n", stderr);
            fflush(stderr);
            return NULL;
        }
	else if (ch == 27) /* ESC */ {
            fputc('\n', stderr);
            fputs(prompt, stderr);
            fflush(stderr);
            n = 0;
        }
        else if ((n < sizeof(password) - 1) && !iscntrl(ch)) {
            password[n++] = ch;
            fputc(' ', stderr);
            fflush(stderr);
        }
	else {
            fputc('\a', stderr);
            fflush(stderr);
        }
    }
 
    fputc('\n', stderr);
    fflush(stderr);
    password[n] = '\0';

    return password;
}

#else

/* linux/hpux/solaris getpass() prototype lives here */
#include <unistd.h>

#include <termios.h>

/* from apr_getpass.c */

#if defined(SIGAR_HPUX)
#   define getpass termios_getpass
#elif defined(SIGAR_SOLARIS)
#   define getpass getpassphrase
#endif

#ifdef SIGAR_HPUX
static char *termios_getpass(const char *prompt)
{
    struct termios attr;
    static char password[MAX_STRING_LEN];
    unsigned int n=0;

    fputs(prompt, stderr);
    fflush(stderr);
        
    if (tcgetattr(STDIN_FILENO, &attr) != 0) {
        return NULL;
    }

    attr.c_lflag &= ~(ECHO);

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &attr) != 0) {
        return NULL;
    }

    while ((password[n] = getchar()) != '\n') {
        if (n < (sizeof(password) - 1) && 
            (password[n] >= ' ') && 
            (password[n] <= '~'))
        {
            n++;
        }
        else {
            fprintf(stderr, "\n");
            fputs(prompt, stderr);
            fflush(stderr);
            n = 0;
        }
    }
 
    password[n] = '\0';
    printf("\n");

    if (n > (MAX_STRING_LEN - 1)) {
        password[MAX_STRING_LEN - 1] = '\0';
    }

    attr.c_lflag |= ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &attr);

    return (char *)&password;
}
#endif

SIGAR_DECLARE(char *) sigar_password_get(const char *prompt)
{
    char *buf = NULL;

    /* the linux version of getpass prints the prompt to the tty; ok.
     * the solaris version prints the prompt to stderr; not ok.
     * so print the prompt to /dev/tty ourselves if possible (always should be)
     */

    FILE *tty = NULL;

    if ((tty = fopen("/dev/tty", "w"))) {
        fprintf(tty, "%s", prompt);
        fflush(tty);

        buf = getpass(tty ? "" : prompt);
        fclose(tty);
    }

    return buf;
}

#endif /* WIN32 */
