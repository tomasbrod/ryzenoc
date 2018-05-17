#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <error.h>

#include <sys/syscall.h>
#include <linux/perf_event.h>

bool f_help, f_stop;
struct timespec time_last_measure;
long unsigned o_period_ms;
unsigned g_ncore;
int g_msrfd[32];

void process_options(int argc, char **argv)
{
  // todo
  f_help=false;
  f_stop=false;
  o_period_ms=2000;
  //o_cpumask=0xAAAA;
  return;
}

bool sleep_between_steps() {
  struct timespec rqtp;
  rqtp.tv_sec = time_last_measure.tv_sec + (o_period_ms/1000);
  rqtp.tv_nsec = time_last_measure.tv_nsec + ((o_period_ms%1000)*1000000);
  if(rqtp.tv_nsec>1000000000) {
    rqtp.tv_nsec-=1000000000;
    rqtp.tv_sec+=1;
  }
  
  if(0==clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &rqtp, NULL))
    return true;
  if(errno==EINTR)
    return false;
  error(2, errno, "clock_nanosleep()");
  return false;
}

/* Cumulative values */
uint64_t prev_wpkg;
double g_energy_unit;
uint64_t prev_mperf[32];
uint64_t prev_aperf[32];

/* Ready values */
double v_ppkg;
unsigned v_mhza;
unsigned v_mhzb;
float v_bzy;


uint64_t read_msr(int cpuno, unsigned long which)
{
	uint64_t data;

	if ( pread(g_msrfd[cpuno], &data, sizeof data, which) != sizeof data ) {
		perror("read_msr:pread(%d,,%d)");
		exit(127);
	}

	return data;
}
static unsigned long long rdtsc(void)
{
	unsigned int low, high;

	asm volatile("rdtsc" : "=a" (low), "=d" (high));

	return low | ((unsigned long long)high) << 32;
}

#define AMD_MSR_PWR_UNIT 0xC0010299
#define AMD_MSR_CORE_ENERGY 0xC001029A
#define AMD_MSR_PACKAGE_ENERGY 0xC001029B
#define MSR_IA32_APERF 0x000000E8
#define MSR_IA32_MPERF 0x000000E7

#define AMD_TIME_UNIT_MASK 0xF0000
#define AMD_ENERGY_UNIT_MASK 0x1F00
#define AMD_POWER_UNIT_MASK 0xF


void init_data() {
  time_last_measure.tv_sec=0;
  time_last_measure.tv_nsec=0;
  g_ncore=0;
  unsigned icpu,cpumask;
  for(cpumask= 0xAAAA, icpu=0; cpumask; cpumask>>=1, ++icpu)
  {
    if(cpumask&1)
    {
      char msr_filename[BUFSIZ];
      int fd;
      sprintf(msr_filename, "/dev/cpu/%d/msr", icpu);
      fd = open(msr_filename, O_RDONLY);
      if(fd!=-1)
      {
        g_msrfd[g_ncore]= fd;
        printf("map cpu %d -> msr %d -> fd %d\n",icpu,g_ncore,fd);
        g_ncore++;
      }
      else
      {
        if(errno==ENOENT)
          continue;
        error(3,errno,"open(%s)",msr_filename);
      }
    }
  }
  printf("Found %d cores to monitor\n",g_ncore);

  unsigned long core_energy_units = read_msr(g_msrfd[0], AMD_MSR_PWR_UNIT);

	g_energy_unit = pow(0.5,(core_energy_units & AMD_ENERGY_UNIT_MASK) >> 8);
  printf("energy unit %g\n",g_energy_unit);
}

void measure() {
  struct timespec time_now;
  if(0!=clock_gettime(CLOCK_MONOTONIC, &time_now))
    error(2, errno, "clock_gettime()");

  unsigned icpu;
  uint64_t aperf[g_ncore];
  uint64_t mperf[g_ncore];

  /* FIXME: Accurate and quick reading of core-local MSRs must be done using
   * pinned threads. */

  /* read all registers */
  uint64_t wpkg_raw = read_msr(0, AMD_MSR_PACKAGE_ENERGY);
  for(icpu=0; icpu<g_ncore; ++icpu) {
    aperf[icpu] = read_msr(icpu, MSR_IA32_APERF);
    mperf[icpu] = read_msr(icpu, MSR_IA32_MPERF);
  }

  /* time taking*/
  double time_passed = (time_now.tv_sec - time_last_measure.tv_sec)
    + (time_now.tv_nsec - time_last_measure.tv_nsec) / 1e9;
  time_last_measure= time_now;

  /* Package Power */
  v_ppkg = ((wpkg_raw - prev_wpkg) * g_energy_unit) / time_passed;
  prev_wpkg= wpkg_raw;
  /* Frequency */
  double aperf_sum=0;
  double mperf_sum=0;
  for(icpu=0; icpu<g_ncore; ++icpu) {
    aperf_sum+= (aperf[icpu]-prev_aperf[icpu]);
    mperf_sum+= (mperf[icpu]-prev_mperf[icpu]);
    prev_mperf[icpu]= mperf[icpu];
    prev_aperf[icpu]= aperf[icpu];
  }
  v_mhza= aperf_sum / (1000000*g_ncore*time_passed);
  //v_mhzb= mperf_sum / (1000000*g_ncore*time_passed);
  //v_bzy=v_mhzb/3000.0;
}

void show_help() {}
void display_header() {
  fputs(" MHzA  MHzB Bzy% PkgWatt TempC\r",stdout);
  fflush(stdout);
}
void display() {
  printf("%5u %5u %4.0f %7.02f %5.1f\n",v_mhza,v_mhzb,v_bzy*100,v_ppkg,71.1);
}


int main(int argc, char **argv) {
  process_options(argc, argv);
  if(f_help) {
    show_help();
    return 1;
  }

  init_data();
  measure();
  while(!f_stop) {
    display_header();
    if(!sleep_between_steps())
      break;
    measure();
    display();
  }
  return 0;
}
