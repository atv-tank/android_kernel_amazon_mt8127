#include "tz_secure_clock.h"
#include <linux/earlysuspend.h>

#ifdef TZ_SECURETIME_SUPPORT


uint32_t TEE_update_gb_time_intee(KREE_SESSION_HANDLE session, KREE_SESSION_HANDLE mem_session)
{
	MTEEC_PARAM param[4];
	uint32_t paramTypes;
	TZ_RESULT ret = TZ_RESULT_SUCCESS;
	uint32_t file_result = GB_TIME_FILE_OK_SIGN;
	struct file *file = NULL;
	UINT64 u8Offset = 0;
	int i = 0;
	DRM_UINT64 time_count64;
	struct timeval tv;
	struct TZ_GB_SECURETIME_INFO secure_time;
	DRM_UINT64 cur_counter;
	pr_err("enter  TEE_update_gb_time_intee \n");
	paramTypes = TZ_ParamTypes3(TZPT_MEM_INPUT, TZPT_MEM_INPUT, TZPT_VALUE_OUTPUT);
	param[0].mem.buffer = (void *) &secure_time;
	param[0].mem.size = sizeof(struct TZ_GB_SECURETIME_INFO);
	param[1].mem.buffer = (void *) &cur_counter;
	param[1].mem.size = sizeof(DRM_UINT64);
	
	file = FILE_Open(GB_TIME_FILE_SAVE_PATH, O_RDWR, S_IRWXU);
	if (file) {
		FILE_Read(file, (void *) &secure_time, sizeof(struct TZ_GB_SECURETIME_INFO), &u8Offset);
		filp_close(file, NULL);
	} else {
		file_result = GB_NO_SECURETD_FILE;
		goto err_read;
	}
	for (i=0;i< sizeof(struct TZ_GB_SECURETIME_INFO);i++)
	pr_err("%02x",((char*)&secure_time)[i]);
	pr_err("\n");
	
	do_gettimeofday(&tv);
	time_count64 = (DRM_UINT64)tv.tv_sec;
	pr_info("tv.tv_sec: %llu\n", time_count64);
	memcpy((char *) &cur_counter, (char *) &time_count64, sizeof(DRM_UINT64));
	
	ret = KREE_TeeServiceCall(session, TZCMD_SECURETIME_SET_CURRENT_COUNTER, paramTypes, param);
	if (ret != TZ_RESULT_SUCCESS)
		pr_info("ServiceCall TZCMD_SECURETIME_SET_CURRENT_COUNTER error %d\n", ret);
	
	if (param[2].value.a == GB_TIME_FILE_ERROR_SIGN) {
		file_result = GB_TIME_FILE_ERROR_SIGN;
		pr_info("ServiceCall TZCMD_SECURETIME_SET_CURRENT_COUNTER file_result %d\n", file_result);
	} else if (param[2].value.a == GB_TIME_FILE_OK_SIGN) {
		file_result = GB_TIME_FILE_OK_SIGN;
		pr_info("ServiceCall TZCMD_SECURETIME_SET_CURRENT_COUNTER file_result %d\n", file_result);
	}
	
	err_read:
	if (file_result == GB_TIME_FILE_OK_SIGN)
		return ret;
	else
		return file_result;


}

uint32_t TEE_update_gb_time_infile(KREE_SESSION_HANDLE session, KREE_SESSION_HANDLE mem_session)
{
uint32_t *shm_p;

MTEEC_PARAM param[4];
uint32_t paramTypes;

TZ_RESULT ret = TZ_RESULT_SUCCESS;
struct file *file = NULL;
UINT64 u8Offset = 0;
pr_err("enter  TEE_update_gb_time_infile \n");

shm_p = kmalloc(sizeof(struct TZ_GB_SECURETIME_INFO), GFP_KERNEL);


paramTypes = TZ_ParamTypes3(TZPT_MEM_OUTPUT, TZPT_VALUE_INPUT, TZPT_VALUE_OUTPUT);
param[0].mem.buffer = shm_p;
param[0].mem.size = sizeof(struct TZ_GB_SECURETIME_INFO);
param[1].value.a = 0;

ret = KREE_TeeServiceCall(session, TZCMD_SECURETIME_GET_CURRENT_COUNTER, paramTypes, param);
if (ret != TZ_RESULT_SUCCESS) {
	pr_err("ServiceCall error %d\n", ret);
	goto tz_error;
}

file = FILE_Open(GB_TIME_FILE_SAVE_PATH, O_RDWR|O_CREAT, S_IRWXU);
if (file) {
	FILE_Write(file, (void *)shm_p, sizeof(struct TZ_GB_SECURETIME_INFO), &u8Offset);
	filp_close(file, NULL);

} else {
	pr_err("FILE_Open GB_TIME_FILE_SAVE_PATH return NULL\n");
}

tz_error:


if (shm_p != NULL)
	kfree(shm_p);

return ret;

}



uint32_t TEE_Icnt_time(KREE_SESSION_HANDLE session, KREE_SESSION_HANDLE mem_session)
{
uint32_t *shm_p;
//KREE_SHAREDMEM_PARAM shm_param;
//KREE_SHAREDMEM_HANDLE shm_handle;
MTEEC_PARAM param[4];
uint32_t paramTypes;
TZ_RESULT ret;
unsigned long time_count = 1392967151;

int err = -ENODEV;
struct timeval tv;

ret = err;
pr_err("enter  TEE_Icnt_time \n");

shm_p = kmalloc(sizeof(struct TM_GB), GFP_KERNEL);

param[1].mem.buffer = shm_p;
param[1].mem.size = sizeof(struct TM_GB);
paramTypes = TZ_ParamTypes2(TZPT_VALUE_INPUT, TZPT_MEM_OUTPUT);
do_gettimeofday(&tv);

time_count = (DRM_UINT64)tv.tv_sec;
// pr_info("tv.tv_sec: %lu\n", time_count);

param[0].value.a = time_count;

ret = KREE_TeeServiceCall(session, TZCMD_SECURETIME_INC_CURRENT_COUNTER, paramTypes, param);
if (ret != TZ_RESULT_SUCCESS)
	pr_err("ServiceCall error %d\n", ret);

#if 0
pr_info("securetime increase result: %d %d %d %d %d %d %d\n", ((struct TM_GB *) shm_p)->tm_yday
	, ((struct TM_GB *) shm_p)->tm_year, ((struct TM_GB *) shm_p)->tm_mon, ((struct TM_GB *) shm_p)->tm_mday
	, ((struct TM_GB *) shm_p)->tm_hour, ((struct TM_GB *) shm_p)->tm_min, ((struct TM_GB *) shm_p)->tm_sec);
#endif


if (shm_p != NULL)
	kfree(shm_p);

return ret;
}

#define THREAD_COUNT_FREQ 5
#define THREAD_SAVEFILE_VALUE (10*60)  //   store secure time per minute
static int check_count;
static KREE_SESSION_HANDLE icnt_session;
static KREE_SESSION_HANDLE mem_session;


static int securetime_savefile_gb(void)
{
int ret = 0;

KREE_SESSION_HANDLE securetime_session = 0;
KREE_SESSION_HANDLE mem_session = 0;

ret = KREE_CreateSession(TZ_TA_SECURETIME_UUID, &securetime_session);
if (ret != TZ_RESULT_SUCCESS) {
		pr_info("[securetime]CreateSession error %d\n", ret);
} else {
ret = KREE_CreateSession(TZ_TA_MEM_UUID, &mem_session);
TEE_update_gb_time_infile(securetime_session, mem_session);

ret = KREE_CloseSession(securetime_session);
if (ret != TZ_RESULT_SUCCESS)
		pr_info("CloseSession error %d\n", ret);
ret = KREE_CloseSession(mem_session);
if (ret != TZ_RESULT_SUCCESS)
		pr_info("[securetime]CloseMEMSession error %d\n", ret);

}
return ret;


}


#ifdef CONFIG_HAS_EARLYSUSPEND_GB

static void st_early_suspend_gb(struct early_suspend *h)
{
	pr_info("[securetime]st_early_suspend_gb error \n");

//pr_info("st_early_suspend: start\n");
	securetime_savefile_gb();

}
static void st_late_resume_gb(struct early_suspend *h)
{
int ret = 0;
KREE_SESSION_HANDLE securetime_session = 0;
KREE_SESSION_HANDLE mem_session = 0;
ret = KREE_CreateSession(TZ_TA_SECURETIME_UUID, &securetime_session);
if (ret != TZ_RESULT_SUCCESS) {
	pr_info("[securetime]CreateSession error %d\n", ret);
} else {
ret = KREE_CreateSession(TZ_TA_MEM_UUID, &mem_session);
TEE_update_gb_time_intee(securetime_session, mem_session);
ret = KREE_CloseSession(securetime_session);
if (ret != TZ_RESULT_SUCCESS)
		pr_info("[securetime]CloseSession error %d\n", ret);
ret = KREE_CloseSession(mem_session);
if (ret != TZ_RESULT_SUCCESS)
		pr_info("[securetime]CloseMEMSession error %d\n", ret);
}
}

static struct early_suspend securetime_early_suspend_gb = {
	.level  = 258,
	.suspend = st_early_suspend_gb,
	.resume  = st_late_resume_gb,
};
#endif




int update_securetime_thread_gb(void *data)
{
TZ_RESULT ret;
unsigned int update_ret = 0;
uint32_t nsec = THREAD_COUNT_FREQ;

ret = KREE_CreateSession(TZ_TA_SECURETIME_UUID, &icnt_session);
if (ret != TZ_RESULT_SUCCESS) {
	pr_err("update_securetime_thread_gb CreateSession error %d\n", ret);
	return 1;
}
ret = KREE_CreateSession(TZ_TA_MEM_UUID, &mem_session);
if (ret != TZ_RESULT_SUCCESS) {
	pr_err("update_securetime_thread_gb Create memory session error %d\n", ret);
	ret = KREE_CloseSession(icnt_session);
	return ret;
}

set_freezable();

schedule_timeout_interruptible(HZ * nsec * 6 * 2);  //  2  minutes 

update_ret = TEE_update_gb_time_intee(icnt_session, mem_session);
if (update_ret == GB_NO_SECURETD_FILE || update_ret == GB_TIME_FILE_ERROR_SIGN) {
	TEE_update_gb_time_infile(icnt_session, mem_session);
	TEE_update_gb_time_intee(icnt_session, mem_session);
}

#ifdef CONFIG_HAS_EARLYSUSPEND_GB

	register_early_suspend(&securetime_early_suspend_gb);
#endif


for (;;) {
		if (kthread_should_stop())
			break;
		if (try_to_freeze())
			continue;
		schedule_timeout_interruptible(HZ * nsec);
		if (icnt_session && mem_session) {
			TEE_Icnt_time(icnt_session, mem_session);
			check_count += THREAD_COUNT_FREQ;
		if ((check_count < 0) || (check_count > THREAD_SAVEFILE_VALUE)) {
			TEE_update_gb_time_infile(icnt_session, mem_session);
			check_count = 0;
		}
}

		// pr_err("update_securetime_thread_gb updata inc count\n");

}

ret = KREE_CloseSession(icnt_session);
if (ret != TZ_RESULT_SUCCESS)
	pr_err("update_securetime_thread_gb CloseSession error %d\n", ret);


ret = KREE_CloseSession(mem_session);
if (ret != TZ_RESULT_SUCCESS)
	pr_err("update_securetime_thread_gb Close memory session error %d\n", ret);

return 0;
}
#endif

