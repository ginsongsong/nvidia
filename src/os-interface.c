/* _NVRM_COPYRIGHT_BEGIN_
 *
 * Copyright 1999-2001 by NVIDIA Corporation.  All rights reserved.  All
 * information contained herein is proprietary and confidential to NVIDIA
 * Corporation.  Any use, reproduction, or disclosure without the written
 * permission of NVIDIA Corporation is prohibited.
 *
 * _NVRM_COPYRIGHT_END_
 */



/******************* Operating System Interface Routines *******************\
*                                                                           *
* Module: interface.c                                                       *
*   This is the OS interface module.  All operating system transactions     *
*   pass through these routines.  No other operating system specific code   *
*   or data should exist in the source.                                     *
*                                                                           *
\***************************************************************************/

#define  __NO_VERSION__
#include <nvidia/nv-misc.h>

#include <nvidia/os-interface.h>
#include <nvidia/nv-linux.h>

#if defined(CONFIG_SMP)
#ifdef NV_OS_SMP_BARRIER_DECL
NV_OS_SMP_BARRIER_DECL
#else /* NV_OS_SMP_BARRIER_DECL */
#define nv_os_smp_barrier_init() (0)

static atomic_t os_smp_barrier = ATOMIC_INIT(0);

static void ipi_handler(void *info)
{
    while (atomic_read(&os_smp_barrier) != 0)
        NV_CPU_RELAX();
}
#endif /* NV_OS_SMP_BARRIER_DECL */
#else /* CONFIG_SMP */
#define nv_os_smp_barrier_init() (0)
#endif /* CONFIG_SMP */

RM_STATUS NV_API_CALL os_raise_smp_barrier(void)
{
    int ret = 0;
    if (!NV_MAY_SLEEP())
    {
        os_dbg_breakpoint();
        return RM_ERR_NOT_SUPPORTED;
    }
#if defined(CONFIG_SMP)
#if defined(NV_OS_SMP_BARRIER_DECL)
    if (os_smp_barrier_raised)
        return RM_ERR_INVALID_STATE;
#else /* NV_OS_SMP_BARRIER_DECL */
    if (atomic_read(&os_smp_barrier) != 0)
        return RM_ERR_INVALID_STATE;
#endif /* NV_OS_SMP_BARRIER_DECL */
#if defined(preempt_disable)
    preempt_disable();
#endif
    local_bh_disable();
#if defined(NV_OS_SMP_BARRIER_DECL)
    ret = nv_os_smp_barrier_raise();
#else /* NV_OS_SMP_BARRIER_DECL */
    atomic_set(&os_smp_barrier, 1);
    ret = NV_SMP_CALL_FUNCTION(ipi_handler, NULL, 0);
#endif /* NV_OS_SMP_BARRIER_DECL */
#endif
    return (ret == 0) ? RM_OK : RM_ERROR;
}

RM_STATUS NV_API_CALL os_clear_smp_barrier(void)
{
#if defined(CONFIG_SMP)
#if defined(NV_OS_SMP_BARRIER_DECL)
    nv_os_smp_barrier_lower();
#else /* NV_OS_SMP_BARRIER_DECL */
    if (atomic_read(&os_smp_barrier) == 0)
        return RM_OK;
    atomic_set(&os_smp_barrier, 0);
#endif /* NV_OS_SMP_BARRIER_DECL */
    local_bh_enable();
#if defined(preempt_enable)
    preempt_enable();
#endif
#endif
    return RM_OK;
}

RM_STATUS NV_API_CALL os_disable_console_access(void)
{
    NV_ACQUIRE_CONSOLE_SEM();
    return RM_OK;
}

RM_STATUS NV_API_CALL os_enable_console_access(void)
{
    NV_RELEASE_CONSOLE_SEM();
    return RM_OK;
}

//
// Contents of opaque structure to implement a more feature rich
// version of counting semaphores below
//
typedef struct os_sema_s
{
    nv_stack_t        *sp;
    struct completion  completion;
    nv_spinlock_t      lock;
    unsigned long      eflags;
    NvS32              count;
} os_sema_t;

//
// os_alloc_sema - Allocate the global RM semaphore
//
//  ppSema - filled in with pointer to opaque structure to semaphore data type
//
RM_STATUS NV_API_CALL os_alloc_sema
(
    void **ppSema
)
{
    RM_STATUS rmStatus;
    os_sema_t *os_sema;
    nv_stack_t *sp = NULL;

    NV_KMEM_CACHE_ALLOC_STACK(sp);
    if (sp == NULL)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: failed to allocate stack!\n");
        return RM_ERR_NO_FREE_MEM;
    }

    rmStatus = os_alloc_mem(ppSema, sizeof(os_sema_t));
    if (rmStatus != RM_OK)
    {
        NV_KMEM_CACHE_FREE_STACK(sp);
        nv_printf(NV_DBG_ERRORS, "NVRM: failed to allocate semaphore!\n");
        return rmStatus;
    }

    os_sema = (os_sema_t *)*ppSema;
    os_sema->sp = sp;
    init_completion(&os_sema->completion);
    NV_SPIN_LOCK_INIT(&os_sema->lock);
    os_sema->eflags = 0;
    os_sema->count = 1;

    if (nv_os_smp_barrier_init())
    {
        os_free_mem(*ppSema);
        NV_KMEM_CACHE_FREE_STACK(sp);
        return RM_ERROR;
    }

    return RM_OK;
}

//
// os_free_sema - Free resources associated with counting semaphore allocated
//                via os_alloc_sema above.  Semaphore must be free
//                (no waiting threads).
//
//  pSema - Pointer to opaque structure to semaphore data type
//
RM_STATUS NV_API_CALL os_free_sema
(
    void  *pSema
)
{
    os_sema_t *os_sema = (os_sema_t *)pSema;
    nv_stack_t *sp;

    sp = os_sema->sp;
    os_free_mem(pSema);
    NV_KMEM_CACHE_FREE_STACK(sp);

    return RM_OK;
}

//
// os_acquire_sema - Perform a P (acquire) operation on the given semaphore
//                   If the semaphore is acquired (transitions to 1 to 0)
//                   clear all device interrupt enables to protect API threads
//                   from interrupt  handlers.
//
//  pSema - Pointer to opaque structure to semaphore data type
//
RM_STATUS NV_API_CALL os_acquire_sema
(
    void  *pSema
)
{
    os_sema_t *os_sema = (os_sema_t *)pSema;
    unsigned long old_irq;

    NV_SPIN_LOCK_IRQSAVE(&os_sema->lock, old_irq);
    if (os_sema->count <= 0)
    {
        os_sema->count--;
        NV_SPIN_UNLOCK_IRQRESTORE(&os_sema->lock, old_irq);
        wait_for_completion(&os_sema->completion);
    }
    else
    {
        os_sema->count--;
        rm_disable_interrupts(os_sema->sp);
        NV_SPIN_UNLOCK_IRQRESTORE(&os_sema->lock, old_irq);
    }

    return RM_OK;
}

//
// os_cond_acquire_sema - Perform a P (acquire) operation on the given semaphore
//                        only if the operation will not fail (sleep).
//                        If the semaphore is acquired (transitions to 1 to 0)
//                        clear all device interrupt enables to protect API
//                        threads from interrupt handlers and return TRUE.
//
//  pSema - Pointer to opaque structure to semaphore data type
//
RM_STATUS NV_API_CALL os_cond_acquire_sema
(
    void  *pSema
)
{
    os_sema_t *os_sema = (os_sema_t *)pSema;
    unsigned long old_irq;

    NV_SPIN_LOCK_IRQSAVE(&os_sema->lock, old_irq);
    if (os_sema->count <= 0)
    {
        NV_SPIN_UNLOCK_IRQRESTORE(&os_sema->lock, old_irq);
        return RM_ERROR;
    }
    else
    {
        os_sema->count--;
        rm_disable_interrupts(os_sema->sp);
        NV_SPIN_UNLOCK_IRQRESTORE(&os_sema->lock, old_irq);
        return RM_OK;
    }

    return RM_ERROR;
}

//
// os_release_sema - Perform a V (release) operation on the given semaphore,
//                   waking up a single waiting client if at least one is
//                   waiting. If the semaphore is freed (transitions
//                   from 0 to 1) re-enable interrupts.
//
//  pSema - Pointer to opaque structure to semaphore data type
//
RM_STATUS NV_API_CALL os_release_sema
(
    void  *pSema
)
{
    os_sema_t *os_sema = (os_sema_t *)pSema;
    unsigned long old_irq;
    BOOL doWakeup;

    NV_SPIN_LOCK_IRQSAVE(&os_sema->lock, old_irq);
    if (os_sema->count < 0)
    {
        doWakeup = TRUE;
    }
    else
    {
        doWakeup = FALSE;
        rm_enable_interrupts(os_sema->sp);
    }
    os_sema->count++;
    NV_SPIN_UNLOCK_IRQRESTORE(&os_sema->lock, old_irq);

    if (doWakeup)
        complete(&os_sema->completion);

    return RM_OK;
}

//
// os_is_acquired_sema - Return TRUE if semaphore is currently acquired.
//                       Useful for assertions that a thread currently owns
//                       the semaphore.
//
//  pOS   - OS object
//  pSema - Pointer to opaque structure to semaphore data type
//
BOOL NV_API_CALL os_is_acquired_sema
(
    void  *pSema
)
{
    os_sema_t *os_sema = (os_sema_t *)pSema;

    return (os_sema->count < 1);
}

// return TRUE if the caller is the super-user
BOOL NV_API_CALL os_is_administrator(
    PHWINFO pDev
)
{
    return NV_IS_SUSER();
}

NvU32 NV_API_CALL os_get_page_size(void)
{
    return PAGE_SIZE;
}

NvU64 NV_API_CALL os_get_page_mask(void)
{
    return PAGE_MASK;
}

NvU64 NV_API_CALL os_get_system_memory_size(void)
{
#if defined(NV_NO_PAGE_STRUCT)
    return ((NvU64) 0);
#else /* NV_NO_PAGE_STRUCT */
    return ((NvU64) num_physpages * PAGE_SIZE) / RM_PAGE_SIZE;
#endif /* NV_NO_PAGE_STRUCT */
}

//
// Some quick and dirty library functions.
// This is an OS function because some operating systems supply their
// own version of this function that they require you to use instead
// of the C library function.  And some OS code developers may decide to
// use the actual C library function instead of this code.  In this case,
// just replace the code within osStringCopy with a call to the C library
// function strcpy.
//
char* NV_API_CALL os_string_copy(
    char *dst,
    const char *src
)
{
    return strcpy(dst, src);
}

RM_STATUS NV_API_CALL os_strncpy_from_user(
    char *dst,
    const char *src,
    NvU32 n
)
{
#if defined(NV_NO_PAGE_STRUCT)
    return copy_from_user(dst, src, n) ? RM_ERR_INVALID_POINTER : RM_OK;
#else /* NV_NO_PAGE_STRUCT */
    return strncpy_from_user(dst, src, n) ? RM_ERR_INVALID_POINTER : RM_OK;
#endif /* NV_NO_PAGE_STRUCT */
}

NvU32 NV_API_CALL os_string_length(
    const char* str
)
{
    return strlen(str);
}

NvU8* NV_API_CALL os_mem_copy(
    NvU8  *dst,
    const NvU8 *src,
    NvU32 length
)
{
    NvU8 *ret = dst;
    NvU32 dwords, bytes;
    
    if (length < 128) 
        ret = memcpy(dst, src, length);

    else 
    {
        dwords = length / sizeof(NvU32);
        bytes = length % sizeof(NvU32);

        while (dwords) 
        {
            *(NvU32 *)dst = *(NvU32 *)src;
            dst += sizeof(NvU32);
            src += sizeof(NvU32);
            dwords--;
        }
        while (bytes) 
        {
            *dst = *src;
            dst++;
            src++;
            bytes--;
        }
    }
    return ret;
}

RM_STATUS NV_API_CALL os_memcpy_from_user(
    void *dst,
    const void* src,
    NvU32 length
)
{
    return copy_from_user(dst, src, length) ? RM_ERR_INVALID_POINTER : RM_OK;
}

RM_STATUS NV_API_CALL os_memcpy_to_user(
    void *dst,
    const void* src,
    NvU32 length
)
{
    return copy_to_user(dst, src, length) ? RM_ERR_INVALID_POINTER : RM_OK;
}

void* NV_API_CALL os_mem_set(
    void* dst,
    NvU8 c,
    NvU32 length
)
{
    return memset(dst, (int)c, length);
}

NvS32 NV_API_CALL os_mem_cmp(
    const NvU8 *buf0,
    const NvU8* buf1,
    NvU32 length
)
{
    return memcmp(buf0, buf1, length);
}


/*
 * Operating System Memory Functions
 *
 * There are 2 interesting aspects of resource manager memory allocations
 * that need special consideration on Linux:
 *
 * 1. They are typically very large, (e.g. single allocations of 164KB)
 *
 * 2. The resource manager assumes that it can safely allocate memory in
 *    interrupt handlers.
 *
 * The first requires that we call vmalloc, the second kmalloc. We decide
 * which one to use at run time, based on the size of the request and the
 * context. Allocations larger than 128KB require vmalloc, in the context
 * of an ISR they fail.
 */

#ifndef NV_NO_PAGE_STRUCT
#define KMALLOC_LIMIT 131072
#endif /* NV_NO_PAGE_STRUCT */

RM_STATUS NV_API_CALL os_alloc_mem(
    void **address,
    NvU32 size
)
{
#ifdef NV_NO_PAGE_STRUCT
    size = (size + (sizeof(void *) - 1)) & ~(sizeof(void *) - 1);
#endif /* NV_NO_PAGE_STRUCT */
    NV_MEM_TRACKING_PAD_SIZE(size);

    if (!NV_MAY_SLEEP()) {
#ifndef NV_NO_PAGE_STRUCT
        if (size <= KMALLOC_LIMIT) {
#endif /* NV_NO_PAGE_STRUCT */
            /*
             * The allocation request can be serviced with the Linux slab
             * allocator; the likelyhood of failure is still fairly high,
             * since kmalloc can't sleep.
             */
            NV_KMALLOC_ATOMIC(*address, size);
#ifndef NV_NO_PAGE_STRUCT
        } else {
            /*
             * If the requested amount of memory exceeds kmalloc's limit,
             * we can't fulfill the request, as vmalloc can not be called
             * from an interrupt context (bottom-half, ISR).
             */
            *address = NULL;
        }
#endif /* NV_NO_PAGE_STRUCT */
    } else {
#ifndef NV_NO_PAGE_STRUCT
        if (size <= KMALLOC_LIMIT)
#endif /* NV_NO_PAGE_STRUCT */
        {
            NV_KMALLOC(*address, size);

            // kmalloc may fail for larger allocations if memory is fragmented
            // in this case, vmalloc stands a better chance of making the 
            // allocation, so give it a shot.
            if (*address == NULL)
            {
                NV_VMALLOC(*address, size, 1);
#ifdef NV_NO_PAGE_STRUCT
                size |= 1;
#endif /* NV_NO_PAGE_STRUCT */
            }
        }
#ifndef NV_NO_PAGE_STRUCT
        else
        {
            NV_VMALLOC(*address, size, 1);
        }
#endif /* NV_NO_PAGE_STRUCT */
    }

    NV_MEM_TRACKING_HIDE_SIZE(address, size);

    return *address ? RM_OK : RM_ERR_NO_FREE_MEM;
}

void NV_API_CALL os_free_mem(void *address)
{
    unsigned long va;
    int size;

    NV_MEM_TRACKING_RETRIEVE_SIZE(address, size);

    va = (unsigned long) address;
#ifdef NV_NO_PAGE_STRUCT
    if ((size & 1) != 0)
    {
        size &= ~1;
        NV_VFREE(address, size);
    }
#else /* NV_NO_PAGE_STRUCT */
    if (va >= VMALLOC_START && va < VMALLOC_END)
    {
        NV_VFREE(address, size);
    }
#endif /* NV_NO_PAGE_STRUCT */
    else
    {
        NV_KFREE(address, size);
    }
}


/*****************************************************************************
*
*   Name: osGetCurrentTime
*
*****************************************************************************/

RM_STATUS NV_API_CALL os_get_current_time(
    NvU32 *seconds,
    NvU32 *useconds
)
{
    struct timeval tm;

    do_gettimeofday(&tm);

    *seconds = tm.tv_sec;
    *useconds = tm.tv_usec;

    return RM_OK;
}

//---------------------------------------------------------------------------
//
//  Misc services.
//
//---------------------------------------------------------------------------

#define NV_MSECS_PER_JIFFIE         (1000 / HZ)
#define NV_MSECS_TO_JIFFIES(msec)   ((msec) * HZ / 1000)
#define NV_USECS_PER_JIFFIE         (1000000 / HZ)
#define NV_USECS_TO_JIFFIES(usec)   ((usec) * HZ / 1000000)

// #define NV_CHECK_DELAY_ACCURACY 1

/*
 * It is generally a bad idea to use udelay() to wait for more than
 * a few milliseconds. Since the caller is most likely not aware of
 * this, we use mdelay() for any full millisecond to be safe.
 */

RM_STATUS NV_API_CALL os_delay_us(NvU32 MicroSeconds)
{
    unsigned long mdelay_safe_msec;
    unsigned long usec;

#ifdef NV_CHECK_DELAY_ACCURACY
    struct timeval tm1, tm2;

    do_gettimeofday(&tm1);
#endif

    if (in_irq() && (MicroSeconds > NV_MAX_ISR_DELAY_US))
        return RM_ERROR;
    
    mdelay_safe_msec = MicroSeconds / 1000;
    if (mdelay_safe_msec)
        mdelay(mdelay_safe_msec);

    usec = MicroSeconds % 1000;
    if (usec)
        udelay(usec);

#ifdef NV_CHECK_DELAY_ACCURACY
    do_gettimeofday(&tm2);
    nv_printf(NV_DBG_ERRORS, "NVRM: osDelayUs %d: 0x%x 0x%x\n",
        MicroSeconds, tm2.tv_sec - tm1.tv_sec, tm2.tv_usec - tm1.tv_usec);
#endif

    return RM_OK;
}

/* 
 * On Linux, a jiffie represents the time passed in between two timer
 * interrupts. The number of jiffies per second (HZ) varies across the
 * supported platforms. On i386, where HZ is 100, a timer interrupt is
 * generated every 10ms; the resolution is a lot better on ia64, where
 * HZ is 1024. NV_MSECS_TO_JIFFIES should be accurate independent of
 * the actual value of HZ; any partial jiffies will be 'floor'ed, the
 * remainder will be accounted for with mdelay().
 */

RM_STATUS NV_API_CALL os_delay(NvU32 MilliSeconds)
{
    unsigned long MicroSeconds;
    unsigned long jiffies;
    unsigned long mdelay_safe_msec;
    struct timeval tm_end, tm_aux;
#ifdef NV_CHECK_DELAY_ACCURACY
    struct timeval tm_start;
#endif

    do_gettimeofday(&tm_aux);
#ifdef NV_CHECK_DELAY_ACCURACY
    tm_start = tm_aux;
#endif

    if (in_irq() && (MilliSeconds > NV_MAX_ISR_DELAY_MS))
        return RM_ERROR;

    if (!NV_MAY_SLEEP()) 
    {
        mdelay(MilliSeconds);
        return RM_OK;
    }

    MicroSeconds = MilliSeconds * 1000;
    tm_end.tv_usec = MicroSeconds;
    tm_end.tv_sec = 0;
    NV_TIMERADD(&tm_aux, &tm_end, &tm_end);

    /* do we have a full jiffie to wait? */
    jiffies = NV_USECS_TO_JIFFIES(MicroSeconds);

    if (jiffies)
    {
        //
        // If we have at least one full jiffy to wait, give up
        // up the CPU; since we may be rescheduled before
        // the requested timeout has expired, loop until less
        // than a jiffie of the desired delay remains.
        //
        current->state = TASK_INTERRUPTIBLE;
        do
        {
            schedule_timeout(jiffies);
            do_gettimeofday(&tm_aux);
            if (NV_TIMERCMP(&tm_aux, &tm_end, <))
            {
                NV_TIMERSUB(&tm_end, &tm_aux, &tm_aux);
                MicroSeconds = tm_aux.tv_usec + tm_aux.tv_sec * 1000000;
            }
            else
                MicroSeconds = 0;
        } while ((jiffies = NV_USECS_TO_JIFFIES(MicroSeconds)) != 0);
    }

    if (MicroSeconds > 1000)
    {
        mdelay_safe_msec = MicroSeconds / 1000;
        mdelay(mdelay_safe_msec);
        MicroSeconds %= 1000;
    }
    if (MicroSeconds)
    {
        udelay(MicroSeconds);
    }
#ifdef NV_CHECK_DELAY_ACCURACY
    do_gettimeofday(&tm_aux);
    timersub(&tm_aux, &tm_start, &tm_aux);
    nv_printf(NV_DBG_ERRORS, "NVRM: osDelay %dmsec: %d.%06dsec\n",
        MilliSeconds, tm_aux.tv_sec, tm_aux.tv_usec);
#endif

    return RM_OK;
}

/* return CPU frequency in MHz */
#if defined(NVCPU_X86) || defined(NVCPU_X86_64)
NvU32 NV_API_CALL os_get_cpu_frequency(void)
{
    NvU64 tsc[2];
    NvU64 tsc_d;

    tsc[0] = nv_rdtsc();
    mdelay(250);
    tsc[1] = nv_rdtsc();

    /* we timed 1/4th a second */
    tsc_d = (tsc[1] - tsc[0]) * 4;

    /* convert from Hz to MHz */
    do_div(tsc_d, 1000000);

    return (NvU32)tsc_d;
}
#endif


NvU32 NV_API_CALL os_get_current_process(void)
{
#ifdef NV_USER_MAP
    return nv_user_map_current_process();
#else /* NV_USER_MAP */
    return current->tgid;
#endif /* NV_USER_MAP */
}

RM_STATUS NV_API_CALL os_get_current_thread(NvU64 *threadId)
{
    if (!NV_MAY_SLEEP())
        *threadId = 0;
    else
        *threadId = (NvU64) current->pid;

    return RM_OK;
}

/*******************************************************************************/
/*                                                                             */
/* Debug and logging utilities follow                                          */
/*                                                                             */
/*******************************************************************************/


// The current debug display level (default to maximum debug level)
NvU32 cur_debuglevel = 0xffffffff;


//
// this is what actually outputs the data.
//
inline void NV_API_CALL out_string(const char *str)
{
#if defined(DEBUG)
    static int was_newline = 0;

    if (NV_NUM_CPUS() > 1 && was_newline)
    {
        printk("%d: %s", get_cpu(), str);
        put_cpu();
    }
    else
#endif
        printk("%s", str);

#if defined(DEBUG)
    if (NV_NUM_CPUS() > 1)
    {
        int len, i;

        len = strlen(str);
        if (len > 5) i = 5;
        else         i = len;
        was_newline = 0;
        while (i >= 0)
        {
            if (str[len - i] == '\n') was_newline = 1;
            i--;
        }
    }
#endif
}    



#define MAX_ERROR_STRING 512

/*
 * nv_printf() prints to the "error" log for the driver.
 * Just like printf, adds no newlines or names
 * Returns the number of characters written.
 */

static char nv_error_string[MAX_ERROR_STRING];

int NV_API_CALL nv_printf(
    NvU32 debuglevel,
    const char *printf_format,
    ...
  )
{
    char *p = nv_error_string;
    va_list arglist;
    int chars_written = 0;

    if (debuglevel >= ((cur_debuglevel>>4)&0x3))
    {
        va_start(arglist, printf_format);
        chars_written = vsprintf(p, printf_format, arglist);
        va_end(arglist);
        out_string(p);
    }

    return chars_written;
}

void NV_API_CALL nv_prints(
    NvU32 debuglevel,
    const char *string
)
{
    char *s = NULL, *p = (char *)string;
    int l = 0, n = 0;

    if (debuglevel >= ((cur_debuglevel>>4)&0x3))
    {
        while ((s = strchr(p, '\n')) != NULL)
        {
            sprintf(nv_error_string, "NVRM: ");
            l = strlen(nv_error_string);
            n = NV_MIN((s - p) + 2, (MAX_ERROR_STRING - l));
            snprintf(nv_error_string + l, n, "%s", p);
            nv_error_string[MAX_ERROR_STRING - 1] = '\0';
            printk("%s", nv_error_string);
            p = s + 1;
        }
    }
}

int NV_API_CALL nv_snprintf(char *buf, unsigned int size, const char *fmt, ...)
{
    va_list arglist;
    int chars_written;

    va_start(arglist, fmt);
    chars_written = vsnprintf(buf, size, fmt, arglist);
    va_end(arglist);
    return chars_written;
}

void NV_API_CALL nv_os_log(int log_level, const char *fmt, void *ap)
{
    char    *sys_log_level;
    int     l;

    switch (log_level) {
    case NV_DBG_INFO:
        sys_log_level = KERN_INFO;
        break;
    case NV_DBG_SETUP:
        sys_log_level = KERN_DEBUG;
        break;
    case NV_DBG_USERERRORS:
        sys_log_level = KERN_NOTICE;
        break;
    case NV_DBG_WARNINGS:
        sys_log_level = KERN_WARNING;
        break;
    case NV_DBG_ERRORS:
    default:
        sys_log_level = KERN_ERR;
        break;
    }

    strcpy(nv_error_string, sys_log_level);
    strcat(nv_error_string, "NVRM: ");
    l = strlen(nv_error_string);
    vsnprintf(nv_error_string + l, MAX_ERROR_STRING - l, fmt, ap);
    nv_error_string[MAX_ERROR_STRING - 1] = 0;

    printk("%s", nv_error_string);
}

BOOL NV_API_CALL os_pci_device_present(
    NvU16 vendor,
    NvU16 device
)
{
    struct pci_dev *dev;

    dev = NV_PCI_GET_DEVICE(vendor, device, NULL);
    if (dev) {
        NV_PCI_DEV_PUT(dev);
        return 1;
    }

    return 0;
}

#define VERIFY_HANDLE(handle,ret) \
    if (handle == NULL) { \
        os_dbg_breakpoint(); \
        return ret; \
    }

void* NV_API_CALL os_pci_init_handle(
    NvU32 domain,
    NvU8  bus,
    NvU8  slot,
    NvU8  function,
    NvU16 *vendor,
    NvU16 *device
)
{
    struct pci_dev *dev;
    unsigned int devfn = PCI_DEVFN(slot, function);
    nv_state_t *nv;

    if (!NV_MAY_SLEEP())
    {
        nv_printf(NV_DBG_ERRORS,
            "NVRM: os_pci_init_handle: invalid context!\n");
    }

    if ((function == 0) && (bus != 255 && slot != 255))
    {
        if ((nv = nv_get_adapter_state(domain, bus, slot)) != NULL)
        {
            if (vendor) *vendor = nv->vendor_id;
            if (device) *device = nv->device_id;
            return nv->handle;
        }
    }

    if (!NV_MAY_SLEEP())
        return NULL;

    dev = NV_PCI_GET_SLOT(domain, bus, devfn);
    if (dev) {
        if (vendor) *vendor = dev->vendor;
        if (device) *device = dev->device;
        NV_PCI_DEV_PUT(dev); /* XXX Fix me! (hotplug) */
    }
    return (void *) dev;
}

NvU8 NV_API_CALL os_pci_read_byte(
    void *handle,
    NvU8 offset
)
{
    NvU8 value;
    VERIFY_HANDLE(handle,-1);
    pci_read_config_byte( (struct pci_dev *) handle, offset, &value);
    return value;
}

NvU16 NV_API_CALL os_pci_read_word(
    void *handle,
    NvU8 offset
)
{
    NvU16 value;
    VERIFY_HANDLE(handle,-1);
    pci_read_config_word( (struct pci_dev *) handle, offset, &value);
    return value;
}

NvU32 NV_API_CALL os_pci_read_dword(
    void *handle,
    NvU8 offset
) 
{
    NvU32 value;
    VERIFY_HANDLE(handle,-1);
    pci_read_config_dword( (struct pci_dev *) handle, offset, (u32 *) &value);
    return value;
}

void NV_API_CALL os_pci_write_byte(
    void *handle,
    NvU8 offset,
    NvU8 value
)
{
    VERIFY_HANDLE(handle,);
    pci_write_config_byte( (struct pci_dev *) handle, offset, value);
}

void NV_API_CALL os_pci_write_word(
    void *handle,
    NvU8 offset,
    NvU16 value
)
{
    VERIFY_HANDLE(handle,);
    pci_write_config_word( (struct pci_dev *) handle, offset, value);
}

void NV_API_CALL os_pci_write_dword(
    void *handle,
    NvU8 offset,
    NvU32 value
)
{
    VERIFY_HANDLE(handle,);
    pci_write_config_dword( (struct pci_dev *) handle, offset, value);
}

void NV_API_CALL os_io_write_byte(
    NvU32 address,
    NvU8 value
)
{
    outb(value, address);
}

void NV_API_CALL os_io_write_word(
    NvU32 address,
    NvU16 value
)
{
    outw(value, address);
}

void NV_API_CALL os_io_write_dword(
    NvU32 address,
    NvU32 value
)
{
    outl(value, address);
}

NvU8 NV_API_CALL os_io_read_byte(
    NvU32 address
)
{
    return inb(address);
}

NvU16 NV_API_CALL os_io_read_word(
    NvU32 address
)
{
    return inw(address);
}

NvU32 NV_API_CALL os_io_read_dword(
    NvU32 address
)
{
    return inl(address);
}


#if (defined(CONFIG_MTRR) && (! defined(NV_NO_PAGE_STRUCT)))
typedef struct wb_mem_t {
    NvU64 start;
    NvU64 size;
    struct wb_mem_t *next;
} wb_mem;

static wb_mem *wb_list = NULL;

int save_wb(NvU64 start, NvU64 size)
{
    struct wb_mem_t *ptr;

    NV_KMALLOC(ptr, sizeof(struct wb_mem_t));
    if (ptr == NULL)
        return 0;

    ptr->start = start;
    ptr->size  = size;
    ptr->next  = wb_list;
    wb_list    = ptr;

    return 1;
}

int del_wb(NvU64 start, NvU64 size)
{
    struct wb_mem_t *ptr;
    struct wb_mem_t *prev;

    prev = ptr = wb_list;

    while (ptr != NULL) {
        if (ptr->start == start && ptr->size == size) {
            if (ptr == wb_list)
                wb_list = ptr->next;
            else
                prev->next = ptr->next;

            NV_KFREE(ptr, sizeof(struct wb_mem_t));
            return 1;
        }
        prev = ptr;
        ptr = prev->next;
    }

    return 0;
}
#endif

/*
 * Don't assume MTRR-specific, as we could add support for PATs to 
 * achieve the same results on a PIII or higher
 */

RM_STATUS NV_API_CALL os_set_mem_range(
    NvU64 start,
    NvU64 size,
    NvU32 mode
)
{
#if (defined(CONFIG_MTRR) && (! defined(NV_NO_PAGE_STRUCT)))
    int err;

    /* for now, we only set memory to write-combined */
    if (mode != NV_MEMORY_WRITECOMBINED)
        return RM_ERROR;

    err = mtrr_add(start, size, MTRR_TYPE_WRCOMB, 0x0);
    if (err < 0)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: cannot write-combine 0x%08x, %dM\n",
            start, size / (1024 * 1024));
        return RM_ERROR;
    }

    save_wb(start, size);

    return RM_OK;
#endif
    return RM_ERROR;
}

RM_STATUS NV_API_CALL os_unset_mem_range(
    NvU64 start,
    NvU64 size
)
{
#if (defined(CONFIG_MTRR) && (! defined(NV_NO_PAGE_STRUCT)))
    if (del_wb(start, size))
    {
        mtrr_del(-1, start, size);
    }
    return RM_OK;
#endif
    return RM_ERROR;
}

/*
 * This needs to remap the AGP Aperture from IO space to kernel space,
 * or at least return a pointer to a kernel space mapping of the Aperture
 * should this also check for Write-Combining??
 */

void* NV_API_CALL os_map_kernel_space(
    NvU64 start,
    NvU64 size_bytes,
    NvU32 mode
)
{
    void *vaddr;

    if (start == 0)
    {
        if (mode != NV_MEMORY_CACHED)
        {
            nv_printf(NV_DBG_ERRORS,
                "NVRM: os_map_kernel_space: won't map address 0x%0llx UC!\n", start);
            return NULL;
        }
        else
            return (void *)PAGE_OFFSET;
    }

    if (!NV_MAY_SLEEP())
    {
        nv_printf(NV_DBG_ERRORS,
            "NVRM: os_map_kernel_space: can't map 0x%0llx, invalid context!\n", start);
        os_dbg_breakpoint();
        return NULL;
    }

#if defined(NVCPU_X86)
    if (start > 0xffffffff)
    {
        nv_printf(NV_DBG_ERRORS,
            "NVRM: os_map_kernel_space: can't map > 32-bit address 0x%0llx!\n", start);
        os_dbg_breakpoint();
        return NULL;
    }
#endif

    switch (mode)
    {
        case NV_MEMORY_CACHED:
            NV_IOREMAP(vaddr, start, size_bytes);
            break;
        case NV_MEMORY_WRITECOMBINED:
            NV_IOREMAP_WC(vaddr, start, size_bytes);
            break;
        case NV_MEMORY_UNCACHED:
        case NV_MEMORY_DEFAULT:
            NV_IOREMAP_NOCACHE(vaddr, start, size_bytes);
            break;
        default:
            nv_printf(NV_DBG_ERRORS,
                "NVRM: os_map_kernel_space: unsupported mode!\n");
            return NULL;
    }

    return vaddr;
}

void NV_API_CALL os_unmap_kernel_space(
    void *addr,
    NvU64 size_bytes
)
{
    if (addr == (void *)PAGE_OFFSET)
        return;

    NV_IOUNMAP(addr, size_bytes);
}

// flush the cpu's cache, uni-processor version
RM_STATUS NV_API_CALL os_flush_cpu_cache()
{
    CACHE_FLUSH();
    return RM_OK;
}

void NV_API_CALL os_flush_cpu_write_combine_buffer()
{
    WRITE_COMBINE_FLUSH();
}

// override initial debug level from registry
void NV_API_CALL os_dbg_init(void)
{
    NvU32 new_debuglevel;
    nv_stack_t *sp = NULL;

    NV_KMEM_CACHE_ALLOC_STACK(sp);
    if (sp == NULL)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: failed to allocate stack!\n");
        return;
    }

    if (RM_OK == rm_read_registry_dword(sp, NULL,
                                        "NVreg",
                                        "ResmanDebugLevel",
                                        &new_debuglevel))
    {
        if (new_debuglevel != (NvU32)~0)
            cur_debuglevel = new_debuglevel;
    }

    NV_KMEM_CACHE_FREE_STACK(sp);
}

void NV_API_CALL os_dbg_set_level(NvU32 new_debuglevel)
{
    nv_printf(NV_DBG_SETUP, "NVRM: Changing debuglevel from 0x%x to 0x%x\n",
        cur_debuglevel, new_debuglevel);
    cur_debuglevel = new_debuglevel;
}

RM_STATUS NV_API_CALL os_schedule(void)
{
    if (NV_MAY_SLEEP())
    {
        set_current_state(TASK_INTERRUPTIBLE);
        schedule_timeout(1);
        return RM_OK;
    }
    else
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: os_schedule: Attempted to yield"
                                 " the CPU while in atomic or interrupt"
                                 " context\n");
        return RM_ERR_ILLEGAL_ACTION;
    }
}

static void os_execute_work_item(
    NV_TASKQUEUE_DATA_T *data
)
{
    nv_work_t *work = NV_TASKQUEUE_UNPACK_DATA(data);
    nv_stack_t *sp = NULL;

    NV_KMEM_CACHE_ALLOC_STACK(sp);
    if (sp == NULL)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: failed to allocate stack!\n");
        return;
    }

    rm_execute_work_item(sp, work->data);

    os_free_mem((void *)work);

    NV_KMEM_CACHE_FREE_STACK(sp);
}

RM_STATUS NV_API_CALL os_queue_work_item(
    void *nv_work
)
{
    RM_STATUS status;
    nv_work_t *work;

    status = os_alloc_mem((void **)&work, sizeof(nv_work_t));

    if (RM_OK != status)
        return status;

    work->data = nv_work;

    NV_TASKQUEUE_INIT(&work->task, os_execute_work_item,
                      (void *)work);
    NV_TASKQUEUE_SCHEDULE(&work->task);

    return RM_OK;
}

RM_STATUS NV_API_CALL os_flush_work_queue(void)
{
    if (NV_MAY_SLEEP())
    {
        NV_TASKQUEUE_FLUSH();
        return RM_OK;
    }
    else
    {
        nv_printf(NV_DBG_ERRORS,
                  "NVRM: os_flush_work_queue: attempted to execute passive"
                  "work from an atomic or interrupt context.\n");
        return RM_ERR_ILLEGAL_ACTION;
    }
}

/* we used to have a global lock that we had to drop so incoming interrupts
 * wouldn't hang the machine waiting for the lock. In the switch to finer
 * grained locking, I've had to remove that code. We don't know which card has
 * a lock or which threw the breakpoint. I should probably scan the list of
 * nv_state_t's and drop any held locks before throwing this breakpoint.
 */
void NV_API_CALL os_dbg_breakpoint(void)
{
#ifdef DEBUG
    out_string("Break\n");

#if defined(CONFIG_X86_REMOTE_DEBUG) || defined(CONFIG_KGDB)
        __asm__ __volatile__ ("int $3");
#elif defined(CONFIG_KDB)
    KDB_ENTER();
#else
    out_string("Skipping INT 3, we don't like kernel panics\n");
#endif

#endif /* DEBUG */
}


NvU32 NV_API_CALL os_get_cpu_count()
{
    return NV_NUM_CPUS();
}

void NV_API_CALL os_register_compatible_ioctl(NvU32 cmd, NvU32 size)
{
#if defined(NVCPU_X86_64) && defined(CONFIG_IA32_EMULATION) && \
  !defined(NV_FILE_OPERATIONS_HAS_COMPAT_IOCTL)
    unsigned int request = _IOWR(NV_IOCTL_MAGIC, cmd, char[size]);
    register_ioctl32_conversion(request, (void *)sys_ioctl);
#endif
}

void NV_API_CALL os_unregister_compatible_ioctl(NvU32 cmd, NvU32 size)
{
#if defined(NVCPU_X86_64) && defined(CONFIG_IA32_EMULATION) && \
  !defined(NV_FILE_OPERATIONS_HAS_COMPAT_IOCTL)
    unsigned int request = _IOWR(NV_IOCTL_MAGIC, cmd, char[size]);
    unregister_ioctl32_conversion(request);
#endif
}

BOOL NV_API_CALL os_pat_supported(void)
{
    return (nv_pat_mode != NV_PAT_MODE_DISABLED);
}

void NV_API_CALL os_dump_stack()
{
#if defined(DEBUG) && (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 4, 20))
    dump_stack();
#endif
}

NvU64 NV_API_CALL os_acquire_spinlock(void *pSema)
{
    os_sema_t *os_sema = (os_sema_t *)pSema;
    unsigned long eflags;

    NV_SPIN_LOCK_IRQSAVE(&os_sema->lock, eflags);
    os_sema->eflags = eflags;

#if defined(NVCPU_X86) || defined(NVCPU_X86_64)
    eflags &= X86_EFLAGS_IF;
#endif
    return eflags;
}

void NV_API_CALL os_release_spinlock(void *pSema, NvU64 oldIrql)
{
    os_sema_t *os_sema = (os_sema_t *)pSema;
    unsigned long eflags;

    eflags = os_sema->eflags;
    os_sema->eflags = 0;
    NV_SPIN_UNLOCK_IRQRESTORE(&os_sema->lock, eflags);
}

RM_STATUS NV_API_CALL os_get_address_space_info(
    NvU64 *userStartAddress,
    NvU64 *userEndAddress,
    NvU64 *kernelStartAddress,
    NvU64 *kernelEndAddress
)
{
    RM_STATUS status = RM_OK;

#if !defined(CONFIG_X86_4G) && !defined(NV_NO_THREAD_INFO)
    *kernelStartAddress = PAGE_OFFSET;
    *kernelEndAddress = (NvUPtr)0xffffffffffffffffULL;
    *userStartAddress = 0;
    *userEndAddress = TASK_SIZE;
#else
    *kernelStartAddress = 0;
    *kernelEndAddress = 0;   /* invalid */
    *userStartAddress = 0;
    *userEndAddress = 0;     /* invalid */
    status = RM_ERR_NOT_SUPPORTED;
#endif
    return status;
}

