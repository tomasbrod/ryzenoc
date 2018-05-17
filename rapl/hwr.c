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

#include <sys/syscall.h>
#include <linux/perf_event.h>

int *G_fd_msr=NULL; //todo
int G_amdsmucfg;

uint64_t read_msr2(int cpuno, unsigned long which)
{
	uint64_t data;

	if ( pread(G_fd_msr[cpuno], &data, sizeof data, which) != sizeof data ) {
		perror("read_msr:pread");
		exit(127);
	}

	return data;
}

uint64_t read_amdsmu(unsigned long ireg)
{
	uint64_t data;
	uint64_t addrreg = ireg;
	
	if ( pwrite(G_amdsmucfg, &addrreg, sizeof addrreg, 0x60) != sizeof addrreg ) {
		perror("read_amdsmu:pwrite");
		exit(127);
	}

	if ( pread(G_amdsmucfg, &data, sizeof data, 0x64) != sizeof data ) {
		perror("read_amdsmu:pread");
		exit(127);
	}

	return data;
}

long read_amdsmu_thmtcon_temp()
{
	uint64_t regval= read_amdsmu(0x00059800);
	long temp = (regval >> 21) * 125;
	return temp; /* in Celsius/1000 */
}

struct amd_svi_plane
{
	int U; /* 0.00001V */
	int I; /* ?? */
	uint64_t plane;
	int vid;
};

struct amd_svi_plane read_amdsmu_svi(int planeno)
{
	struct amd_svi_plane ui;
	uint64_t plane= read_amdsmu(0x0005A00C+(planeno*4));
	ui.U= (248 - ((plane >> 16) & 0xff)) * 625;
	//ui.U= (1.55 - (((plane >> 16) & 0xff) * 0.00625))*100000;
	ui.vid=(plane >> 16) & 0x1ff;
	ui.I= (plane & 0xff);
	ui.plane=plane;
	return ui;
}

#define AMD_MSR_PWR_UNIT 0xC0010299
#define AMD_MSR_CORE_ENERGY 0xC001029A
#define AMD_MSR_PACKAGE_ENERGY 0xC001029B
#define MSR_IA32_APERF 0x000000E8
#define MSR_IA32_MPERF 0x000000E7

#define AMD_TIME_UNIT_MASK 0xF0000
#define AMD_ENERGY_UNIT_MASK 0x1F00
#define AMD_POWER_UNIT_MASK 0xF

static int total_cores=0,total_packages=0;
static int package_map[MAX_PACKAGES];

static int detect_packages(void) {

	char filename[BUFSIZ];
	FILE *fff;
	int package;
	int i;

	for(i=0;i<MAX_PACKAGES;i++) package_map[i]=-1;

	printf("\t");
	for(i=0;i<MAX_CPUS;i++) {
		sprintf(filename,"/sys/devices/system/cpu/cpu%d/topology/physical_package_id",i);
		fff=fopen(filename,"r");
		if (fff==NULL) break;
		fscanf(fff,"%d",&package);
		printf("%d (%d)",i,package);
		if (i%8==7) printf("\n\t"); else printf(", ");
		fclose(fff);

		if (package_map[package]==-1) {
			total_packages++;
			package_map[package]=i;
		}

	}

	printf("\n");

	total_cores=i;

	printf("\tDetected %d cores in %d packages\n\n",
		total_cores,total_packages);

	return 0;
}

static int open_msr(int core) {

	char msr_filename[BUFSIZ];
	int fd;

	sprintf(msr_filename, "/dev/cpu/%d/msr", core);
	fd = open(msr_filename, O_RDONLY);
	if ( fd < 0 ) {
		if ( errno == ENXIO ) {
			fprintf(stderr, "rdmsr: No CPU %d\n", core);
			exit(2);
		} else if ( errno == EIO ) {
			fprintf(stderr, "rdmsr: CPU %d doesn't support MSRs\n",
					core);
			exit(3);
		} else {
			perror("rdmsr:open");
			fprintf(stderr,"Trying to open %s\n",msr_filename);
			exit(127);
		}
	}

	return fd;
}

static long long read_msr(int fd, unsigned int which) {

	uint64_t data;

	if ( pread(fd, &data, sizeof data, which) != sizeof data ) {
		perror("rdmsr:pread");
		exit(127);
	}

	return (long long)data;
}

static int rapl_msr_amd_core() {
	unsigned int time_unit, energy_unit, power_unit;
	double time_unit_d, energy_unit_d, power_unit_d;
	
	double *core_energy = (double*)malloc(sizeof(double)*total_cores/2);
	double *core_energy_delta = (double*)malloc(sizeof(double)*total_cores/2);

	double *package = (double*)malloc(sizeof(double)*total_cores/2);
	double *package_delta = (double*)malloc(sizeof(double)*total_cores/2);
	
	int *fd = (int*)malloc(sizeof(int)*total_cores/2);
	
	for (int i = 0; i < total_cores/2; i++) {
		fd[i] = open_msr(i);
	}
	
	int core_energy_units = read_msr(fd[0], AMD_MSR_PWR_UNIT);
	printf("Core energy units: %x\n",core_energy_units);
	
	time_unit = (core_energy_units & AMD_TIME_UNIT_MASK) >> 16;
	energy_unit = (core_energy_units & AMD_ENERGY_UNIT_MASK) >> 8;
	power_unit = (core_energy_units & AMD_POWER_UNIT_MASK);
	printf("Time_unit:%d, Energy_unit: %d, Power_unit: %d\n", time_unit, energy_unit, power_unit);
	
	time_unit_d = pow(0.5,(double)(time_unit));
	energy_unit_d = pow(0.5,(double)(energy_unit));
	power_unit_d = pow(0.5,(double)(power_unit));
	printf("Time_unit:%g, Energy_unit: %g, Power_unit: %g\n", time_unit_d, energy_unit_d, power_unit_d);
	
	int core_energy_raw;
	int package_raw;
	// Read per core energy values
	for (int i = 0; i < total_cores/2; i++) {
		core_energy_raw = read_msr(fd[i], AMD_MSR_CORE_ENERGY);
		package_raw = read_msr(fd[i], AMD_MSR_PACKAGE_ENERGY);

		core_energy[i] = core_energy_raw * energy_unit_d;
		package[i] = package_raw * energy_unit_d;
	}

	usleep(100000);
	for (int i = 0; i < total_cores/2; i++) {
		core_energy_raw = read_msr(fd[i], AMD_MSR_CORE_ENERGY);
		package_raw = read_msr(fd[i], AMD_MSR_PACKAGE_ENERGY);

		core_energy_delta[i] = core_energy_raw * energy_unit_d;
		package_delta[i] = package_raw * energy_unit_d;
	}

	double sum = 0;
	for(int i = 0; i < total_cores/2; i++) {
		double diff = (core_energy_delta[i] - core_energy[i])*10;
		sum += diff;
		printf("Core %d, energy used: %gW, Package: %gW\n", i, diff,(package_delta[i]-package[i])*10);
	}
	
	printf("Core sum: %gW\n", sum);
	
	free(core_energy);
	free(core_energy_delta);
	free(package);
	free(package_delta);
	free(fd);
	
	return 0;
}

int main(int argc, char **argv) {

	G_amdsmucfg = open("/sys/bus/pci/devices/0000:00:00.0/config", O_RDWR);
	if(G_amdsmucfg<0)
	{
		perror("open");
		exit(1);
	}
	printf("thmtcon_temp = %g\n",read_amdsmu_thmtcon_temp()*0.001);
	struct amd_svi_plane svipl[2];
	svipl[0]= read_amdsmu_svi(0);
	svipl[1]= read_amdsmu_svi(1);
	printf("svi CPU %g V (%#05x) %g ?A\n",svipl[0].U*0.00001,svipl[0].vid,svipl[0].I*1.0);
	printf("svi SoC %g V (%#05x) %g ?A\n",svipl[1].U*0.00001,svipl[1].vid,svipl[1].I*1.0);
	/*
	for(int i=0; i<8; i++)
	{
		struct amd_svi_plane svip;
		svip= read_amdsmu_svi(i);
		printf("svi %d %g V %g ?A %#010x\n",i,svip.U*0.00001,svip.I*1.0,svip.plane);
		sleep(1);
	}
	*/
	detect_packages();
	rapl_msr_amd_core();
	
	return 0;
}
