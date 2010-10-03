/*
 * Copyright (c) 2000-2009 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 * 
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * @OSF_COPYRIGHT@
 */

/*
 *	File:		i386/rtclock.c
 *	Purpose:	Routines for handling the machine dependent
 *			real-time clock. Historically, this clock is
 *			generated by the Intel 8254 Programmable Interval
 *			Timer, but local apic timers are now used for
 *			this purpose with the master time reference being
 *			the cpu clock counted by the timestamp MSR.
 */

#include <platforms.h>
#include <mach_kdb.h>

#include <mach/mach_types.h>

#include <kern/cpu_data.h>
#include <kern/cpu_number.h>
#include <kern/clock.h>
#include <kern/host_notify.h>
#include <kern/macro_help.h>
#include <kern/misc_protos.h>
#include <kern/spl.h>
#include <kern/assert.h>
#include <kern/etimer.h>
#include <mach/vm_prot.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>		/* for kernel_map */
#include <i386/ipl.h>
#include <architecture/i386/pio.h>
#include <i386/machine_cpu.h>
#include <i386/cpuid.h>
#include <i386/cpu_threads.h>
#include <i386/mp.h>
#include <i386/machine_routines.h>
#include <i386/proc_reg.h>
#include <i386/misc_protos.h>
#include <i386/lapic.h>
#include <pexpert/pexpert.h>
#include <machine/limits.h>
#include <machine/commpage.h>
#include <sys/kdebug.h>
#include <i386/tsc.h>
#include <i386/rtclock.h>

#define NSEC_PER_HZ			(NSEC_PER_SEC / 100) /* nsec per tick */

#define UI_CPUFREQ_ROUNDING_FACTOR	10000000

int		rtclock_config(void);

int		rtclock_init(void);

uint64_t	rtc_decrementer_min;

uint64_t	tsc_rebase_abs_time = 0;

void			rtclock_intr(x86_saved_state_t *regs);
static uint64_t		maxDec;			/* longest interval our hardware timer can handle (nsec) */

static void	rtc_set_timescale(uint64_t cycles);
static uint64_t	rtc_export_speed(uint64_t cycles);

rtc_nanotime_t	rtc_nanotime_info = {0,0,0,0,1,0};

/*
 * tsc_to_nanoseconds:
 *
 * Basic routine to convert a raw 64 bit TSC value to a
 * 64 bit nanosecond value.  The conversion is implemented
 * based on the scale factor and an implicit 32 bit shift.
 */
static inline uint64_t
_tsc_to_nanoseconds(uint64_t value)
{
#if defined(__i386__)
    noasm("movl	%%edx,%%esi	;"
		 "mull	%%ecx		;"
		 "movl	%%edx,%%edi	;"
		 "movl	%%esi,%%eax	;"
		 "mull	%%ecx		;"
		 "addl	%%edi,%%eax	;"	
		 "adcl	$0,%%edx	 "
		 : "+A" (value)
		 : "c" (current_cpu_datap()->cpu_nanotime->scale)
		 : "esi", "edi");
#elif defined(__x86_64__)
    noasm("mul %%rcx;"
		 "shrq $32, %%rax;"
		 "shlq $32, %%rdx;"
		 "orq %%rdx, %%rax;"
		 : "=a"(value)
		 : "a"(value), "c"(rtc_nanotime_info.scale)
		 : "rdx", "cc" );
#else
#error Unsupported architecture
#endif

    return (value);
}

static inline uint32_t
_absolutetime_to_microtime(uint64_t abstime, clock_sec_t *secs, clock_usec_t *microsecs)
{
	uint32_t remain;
#if defined(__i386__)
	noasm(
			"divl %3"
				: "=a" (*secs), "=d" (remain)
				: "A" (abstime), "r" (NSEC_PER_SEC));
	noasm(
			"divl %3"
				: "=a" (*microsecs)
				: "0" (remain), "d" (0), "r" (NSEC_PER_USEC));
#elif defined(__x86_64__)
	*secs = abstime / (uint64_t)NSEC_PER_SEC;
	remain = (uint32_t)(abstime % (uint64_t)NSEC_PER_SEC);
	*microsecs = remain / NSEC_PER_USEC;
#else
#error Unsupported architecture
#endif
	return remain;
}

static inline void
_absolutetime_to_nanotime(uint64_t abstime, clock_sec_t *secs, clock_usec_t *nanosecs)
{
#if defined(__i386__)
	noasm(
			"divl %3"
			: "=a" (*secs), "=d" (*nanosecs)
			: "A" (abstime), "r" (NSEC_PER_SEC));
#elif defined(__x86_64__)
	*secs = abstime / (uint64_t)NSEC_PER_SEC;
	*nanosecs = (clock_usec_t)(abstime % (uint64_t)NSEC_PER_SEC);
#else
#error Unsupported architecture
#endif
}

static uint32_t
deadline_to_decrementer(
	uint64_t	deadline,
	uint64_t	now)
{
	uint64_t	delta;

	if (deadline <= now)
		return (uint32_t)rtc_decrementer_min;
	else {
		delta = deadline - now;
		return (uint32_t)MIN(MAX(rtc_decrementer_min,delta),maxDec); 
	}
}

void
rtc_lapic_start_ticking(void)
{
	x86_lcpu_t	*lcpu = x86_lcpu();

	/*
	 * Force a complete re-evaluation of timer deadlines.
	 */
	lcpu->rtcPop = EndOfAllTime;
	etimer_resync_deadlines();
}

/*
 * Configure the real-time clock device. Return success (1)
 * or failure (0).
 */

int
rtclock_config(void)
{
	/* nothing to do */
	return (1);
}


/*
 * Nanotime/mach_absolutime_time
 * -----------------------------
 * The timestamp counter (TSC) - which counts cpu clock cycles and can be read
 * efficiently by the kernel and in userspace - is the reference for all timing.
 * The cpu clock rate is platform-dependent and may stop or be reset when the
 * processor is napped/slept.  As a result, nanotime is the software abstraction
 * used to maintain a monotonic clock, adjusted from an outside reference as needed.
 *
 * The kernel maintains nanotime information recording:
 * 	- the ratio of tsc to nanoseconds
 *	  with this ratio expressed as a 32-bit scale and shift
 *	  (power of 2 divider);
 *	- { tsc_base, ns_base } pair of corresponding timestamps.
 *
 * The tuple {tsc_base, ns_base, scale, shift} is exported in the commpage 
 * for the userspace nanotime routine to read.
 *
 * All of the routines which update the nanotime data are non-reentrant.  This must
 * be guaranteed by the caller.
 */
static inline void
rtc_nanotime_set_commpage(rtc_nanotime_t *rntp)
{
	commpage_set_nanotime(rntp->tsc_base, rntp->ns_base, rntp->scale, rntp->shift);
}

/*
 * rtc_nanotime_init:
 *
 * Intialize the nanotime info from the base time.
 */
static inline void
_rtc_nanotime_init(rtc_nanotime_t *rntp, uint64_t base)
{
	uint64_t	tsc = rdtsc64();

	_rtc_nanotime_store(tsc, base, rntp->scale, rntp->shift, rntp);
}

static void
rtc_nanotime_init(uint64_t base)
{
	rtc_nanotime_t	*rntp = current_cpu_datap()->cpu_nanotime;

	_rtc_nanotime_init(rntp, base);
	rtc_nanotime_set_commpage(rntp);
}

/*
 * rtc_nanotime_init_commpage:
 *
 * Call back from the commpage initialization to
 * cause the commpage data to be filled in once the
 * commpages have been created.
 */
void
rtc_nanotime_init_commpage(void)
{
	spl_t			s = splclock();

	rtc_nanotime_set_commpage(current_cpu_datap()->cpu_nanotime);

	splx(s);
}

/*
 * rtc_nanotime_read:
 *
 * Returns the current nanotime value, accessable from any
 * context.
 */
static inline uint64_t
rtc_nanotime_read(void)
{
	
#if CONFIG_EMBEDDED
	if (gPEClockFrequencyInfo.timebase_frequency_hz > SLOW_TSC_THRESHOLD)
		return	_rtc_nanotime_read(current_cpu_datap()->cpu_nanotime, 1);	/* slow processor */
	else
#endif
	return	_rtc_nanotime_read(current_cpu_datap()->cpu_nanotime, 0);	/* assume fast processor */
}

/*
 * rtc_clock_napped:
 *
 * Invoked from power management when we exit from a low C-State (>= C4)
 * and the TSC has stopped counting.  The nanotime data is updated according
 * to the provided value which represents the new value for nanotime.
 */
void
rtc_clock_napped(uint64_t base, uint64_t tsc_base)
{
	rtc_nanotime_t	*rntp = current_cpu_datap()->cpu_nanotime;
	uint64_t	oldnsecs;
	uint64_t	newnsecs;
	uint64_t	tsc;

	assert(!ml_get_interrupts_enabled());
	tsc = rdtsc64();
	oldnsecs = rntp->ns_base + _tsc_to_nanoseconds(tsc - rntp->tsc_base);
	newnsecs = base + _tsc_to_nanoseconds(tsc - tsc_base);
	
	/*
	 * Only update the base values if time using the new base values
	 * is later than the time using the old base values.
	 */
	if (oldnsecs < newnsecs) {
	    _rtc_nanotime_store(tsc_base, base, rntp->scale, rntp->shift, rntp);
	    rtc_nanotime_set_commpage(rntp);
	}
}

void
rtc_clock_stepping(__unused uint32_t new_frequency,
		   __unused uint32_t old_frequency)
{
	panic("rtc_clock_stepping unsupported");
}

void
rtc_clock_stepped(__unused uint32_t new_frequency,
		  __unused uint32_t old_frequency)
{
	panic("rtc_clock_stepped unsupported");
}

/*
 * rtc_sleep_wakeup:
 *
 * Invoked from power manageent when we have awoken from a sleep (S3)
 * and the TSC has been reset.  The nanotime data is updated based on
 * the passed in value.
 *
 * The caller must guarantee non-reentrancy.
 */
void
rtc_sleep_wakeup(
	uint64_t		base)
{
	/*
	 * Reset nanotime.
	 * The timestamp counter will have been reset
	 * but nanotime (uptime) marches onward.
	 */
	rtc_nanotime_init(base);
}

/*
 * Initialize the real-time clock device.
 * In addition, various variables used to support the clock are initialized.
 */
int
rtclock_init(void)
{
	uint64_t	cycles;

	assert(!ml_get_interrupts_enabled());

	if (cpu_number() == master_cpu) {

		assert(tscFreq);
		rtc_set_timescale(tscFreq);

		/*
		 * Adjust and set the exported cpu speed.
		 */
		cycles = rtc_export_speed(tscFreq);

		/*
		 * Set min/max to actual.
		 * ACPI may update these later if speed-stepping is detected.
		 */
		gPEClockFrequencyInfo.cpu_frequency_min_hz = cycles;
		gPEClockFrequencyInfo.cpu_frequency_max_hz = cycles;

		/*
		 * Compute the longest interval we can represent.
		 */
		maxDec = tmrCvt(0x7fffffffULL, busFCvtt2n);
		kprintf("maxDec: %lld\n", maxDec);

		/* Minimum interval is 1usec */
		rtc_decrementer_min = deadline_to_decrementer(NSEC_PER_USEC, 0ULL);
		/* Point LAPIC interrupts to hardclock() */
		lapic_set_timer_func((i386_intr_func_t) rtclock_intr);

		clock_timebase_init();
		ml_init_lock_timeout();
	}

	rtc_lapic_start_ticking();

	return (1);
}

// utility routine 
// Code to calculate how many processor cycles are in a second...

static void
rtc_set_timescale(uint64_t cycles)
{
	rtc_nanotime_t	*rntp = current_cpu_datap()->cpu_nanotime;
	rntp->scale = (uint32_t)(((uint64_t)NSEC_PER_SEC << 32) / cycles);

	if (cycles <= SLOW_TSC_THRESHOLD)
		rntp->shift = (uint32_t)cycles;
	else
		rntp->shift = 32;

	if (tsc_rebase_abs_time == 0)
		tsc_rebase_abs_time = mach_absolute_time();

	rtc_nanotime_init(0);
}

static uint64_t
rtc_export_speed(uint64_t cyc_per_sec)
{
	uint64_t	cycles;

	/* Round: */
        cycles = ((cyc_per_sec + (UI_CPUFREQ_ROUNDING_FACTOR/2))
			/ UI_CPUFREQ_ROUNDING_FACTOR)
				* UI_CPUFREQ_ROUNDING_FACTOR;

	/*
	 * Set current measured speed.
	 */
        if (cycles >= 0x100000000ULL) {
            gPEClockFrequencyInfo.cpu_clock_rate_hz = 0xFFFFFFFFUL;
        } else {
            gPEClockFrequencyInfo.cpu_clock_rate_hz = (unsigned long)cycles;
        }
        gPEClockFrequencyInfo.cpu_frequency_hz = cycles;

	kprintf("[RTCLOCK] frequency %llu (%llu)\n", cycles, cyc_per_sec);
	return(cycles);
}

void
clock_get_system_microtime(
	clock_sec_t			*secs,
	clock_usec_t		*microsecs)
{
	uint64_t	now = rtc_nanotime_read();

	_absolutetime_to_microtime(now, secs, microsecs);
}

void
clock_get_system_nanotime(
	clock_sec_t			*secs,
	clock_nsec_t		*nanosecs)
{
	uint64_t	now = rtc_nanotime_read();

	_absolutetime_to_nanotime(now, secs, nanosecs);
}

void
clock_gettimeofday_set_commpage(
	uint64_t				abstime,
	uint64_t				epoch,
	uint64_t				offset,
	clock_sec_t				*secs,
	clock_usec_t			*microsecs)
{
	uint64_t	now = abstime + offset;
	uint32_t	remain;

	remain = _absolutetime_to_microtime(now, secs, microsecs);

	*secs += (clock_sec_t)epoch;

	commpage_set_timestamp(abstime - remain, *secs);
}

void
clock_timebase_info(
	mach_timebase_info_t	info)
{
	info->numer = info->denom =  1;
}	

/*
 * Real-time clock device interrupt.
 */
void
rtclock_intr(
	x86_saved_state_t	*tregs)
{
        uint64_t	rip;
	boolean_t	user_mode = FALSE;
	uint64_t	abstime;
	uint32_t	latency;
	x86_lcpu_t	*lcpu = x86_lcpu();

	assert(get_preemption_level() > 0);
	assert(!ml_get_interrupts_enabled());

	abstime = rtc_nanotime_read();
	latency = (uint32_t)(abstime - lcpu->rtcDeadline);
	if (abstime < lcpu->rtcDeadline)
		latency = 1;

	if (is_saved_state64(tregs) == TRUE) {
	        x86_saved_state64_t	*regs;
		  
		regs = saved_state64(tregs);

		if (regs->isf.cs & 0x03)
			user_mode = TRUE;
		rip = regs->isf.rip;
	} else {
	        x86_saved_state32_t	*regs;

		regs = saved_state32(tregs);

		if (regs->cs & 0x03)
		        user_mode = TRUE;
		rip = regs->eip;
	}

	/* Log the interrupt service latency (-ve value expected by tool) */
	KERNEL_DEBUG_CONSTANT(
		MACHDBG_CODE(DBG_MACH_EXCP_DECI, 0) | DBG_FUNC_NONE,
		-(int32_t)latency, (uint32_t)rip, user_mode, 0, 0);

	/* call the generic etimer */
	etimer_intr(user_mode, rip);
}

/*
 *	Request timer pop from the hardware 
 */


int
setPop(
	uint64_t time)
{
	uint64_t now;
	uint32_t decr;
	uint64_t count;
	
	now = rtc_nanotime_read();		/* The time in nanoseconds */
	decr = deadline_to_decrementer(time, now);

	count = tmrCvt(decr, busFCvtn2t);
	lapic_set_timer(TRUE, one_shot, divide_by_1, (uint32_t) count);

	return decr;				/* Pass back what we set */
}


uint64_t
mach_absolute_time(void)
{
	return rtc_nanotime_read();
}

void
clock_interval_to_absolutetime_interval(
	uint32_t		interval,
	uint32_t		scale_factor,
	uint64_t		*result)
{
	*result = (uint64_t)interval * scale_factor;
}

void
absolutetime_to_microtime(
	uint64_t			abstime,
	clock_sec_t			*secs,
	clock_usec_t		*microsecs)
{
	_absolutetime_to_microtime(abstime, secs, microsecs);
}

void
absolutetime_to_nanotime(
	uint64_t			abstime,
	clock_sec_t			*secs,
	clock_nsec_t		*nanosecs)
{
	_absolutetime_to_nanotime(abstime, secs, nanosecs);
}

void
nanotime_to_absolutetime(
	clock_sec_t			secs,
	clock_nsec_t		nanosecs,
	uint64_t			*result)
{
	*result = ((uint64_t)secs * NSEC_PER_SEC) + nanosecs;
}

void
absolutetime_to_nanoseconds(
	uint64_t		abstime,
	uint64_t		*result)
{
	*result = abstime;
}

void
nanoseconds_to_absolutetime(
	uint64_t		nanoseconds,
	uint64_t		*result)
{
	*result = nanoseconds;
}

void
machine_delay_until(
	uint64_t		deadline)
{
	uint64_t		now;

	do {
		cpu_pause();
		now = mach_absolute_time();
	} while (now < deadline);
}
