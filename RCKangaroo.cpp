// This file is a part of RCKangaroo software
// (c) 2024, RetiredCoder (RC)
// License: GPLv3, see "LICENSE.TXT" file
// https://github.com/RetiredC


#include <iostream>
#include <vector>
#include <deque>

#include "cuda_runtime.h"
#include "cuda.h"

#include "defs.h"
#include "utils.h"
#include "GpuKang.h"


EcJMP EcJumps1[JMP_CNT];
EcJMP EcJumps2[JMP_CNT];
EcJMP EcJumps3[JMP_CNT];

RCGpuKang* GpuKangs[MAX_GPU_CNT];
int GpuCnt;
volatile long ThrCnt;
volatile bool gSolved;

EcInt Int_HalfRange;
EcPoint Pnt_HalfRange;
EcPoint Pnt_NegHalfRange;
EcInt Int_TameOffset;
Ec ec;

CriticalSection csAddPoints;
CriticalSection csCheckpoints;
u8* pPntList;
u8* pPntList2;
volatile int PntIndex;
TFastBase db;
EcPoint gPntToSolve;
EcInt gPrivKey;

volatile u64 TotalOps;
u32 TotalSolved;
u32 gTotalErrors;
u64 PntTotalOps;
bool IsBench;

u32 gDP;
u32 gRange;
EcInt gStart;
bool gStartSet;
EcPoint gPubKey;
u8 gGPUs_Mask[MAX_GPU_CNT];
char gTamesFileName[1024];
double gMax;
bool gGenMode; //tames generation mode
bool gIsOpsLimit;

bool gSaveCheckpoints = false;
std::string gClientID;
std::string gSoftVersion = "3.52";
int gLastCheckpointDay = -1;
std::string gRawParams;
#include <ctime>
#include <cstdio>
#include <string>
#include <sstream>
#include <iomanip>
#include <functional>
#include <fstream>
#include <vector>
#include <curl/curl.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#endif

char gCheckpointFileName[1024] = {0};
static std::vector<std::string> gPoolCheckpoints;
static size_t gSavedLines = 0;
std::string gMachineId;
std::string gMachineIdHash4;
std::string gParamsHash4;
std::string gPoolAddress;

struct RemoteCheckpointLine
{
	std::string filename;
	std::string line;
};

static std::deque<RemoteCheckpointLine> gPendingRemote;
static bool gCurlInitialized = false;

static void CurlGlobalCleanup()
{
	if (gCurlInitialized)
	{
		curl_global_cleanup();
		gCurlInitialized = false;
	}
}

static bool EnsureCurlInitialized()
{
	if (gPoolAddress.empty())
		return false;

	if (gCurlInitialized)
		return true;

	if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
	{
		printf("Error: failed to initialize cURL. Remote checkpoint sync disabled.\n");
		return false;
	}

	if (atexit(CurlGlobalCleanup) != 0)
	{
		printf("Warning: failed to register cURL cleanup handler.\n");
	}

	gCurlInitialized = true;
	return true;
}

static std::string JsonEscape(const std::string &input)
{
	std::string out;
	out.reserve(input.size() + 8);
	for (unsigned char c : input)
	{
		switch (c)
		{
		case '\\':
			out += "\\\\";
			break;
		case '\"':
			out += "\\\"";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20)
			{
				char buf[7];
				sprintf(buf, "\\u%04x", c);
				out += buf;
			}
			else
				out += (char)c;
			break;
		}
	}
	return out;
}

static bool SendRemoteLine(const RemoteCheckpointLine &entry)
{
	if (!EnsureCurlInitialized())
		return false;

	CURL *curl = curl_easy_init();
	if (!curl)
	{
		printf("Warning: cannot initialize cURL easy handle for checkpoint sync.\n");
		return false;
	}

	std::string payload = std::string("{\"filename\":\"") + JsonEscape(entry.filename) + "\",\"line\":\"" + JsonEscape(entry.line) + "\"}";

	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");

	curl_easy_setopt(curl, CURLOPT_URL, gPoolAddress.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, payload.size());
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "RCKangaroo/3.52");

	CURLcode res = curl_easy_perform(curl);
	long response_code = 0;
	if (res == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK)
	{
		printf("Warning: failed to send checkpoint to pool (%s).\n", curl_easy_strerror(res));
		return false;
	}

	if (response_code < 200 || response_code >= 300)
	{
		printf("Warning: pool server returned HTTP %ld while saving checkpoint.\n", response_code);
		return false;
	}

	return true;
}

static void TryFlushRemoteQueue()
{
	if (gPoolAddress.empty())
		return;

	while (!gPendingRemote.empty())
	{
		if (!SendRemoteLine(gPendingRemote.front()))
			break;
		gPendingRemote.pop_front();
	}
}

static void QueueRemoteLine(const std::string &filename, const std::string &line)
{
	if (gPoolAddress.empty() || filename.empty())
		return;

	gPendingRemote.push_back(RemoteCheckpointLine{filename, line});
}

void InitMachineIdHash();
void InitParamsHash();

static inline uint32_t FNV1a16(const std::string &s)
{
	uint32_t h = 2166136261u;
	for (unsigned char c : s)
	{
		h ^= c;
		h *= 16777619u;
	}
	return h & 0xFFFFu;
}

static inline std::string Norm(std::string v)
{
	for (auto &ch : v)
	{
		if (ch == '\\')
			ch = '/';
		ch = (char)std::tolower((unsigned char)ch);
	}
	return v;
}

//

#pragma pack(push, 1)
struct DBRec
{
	u8 x[12];
	u8 d[22];
	u8 type; //0 - tame, 1 - wild1, 2 - wild2
};
#pragma pack(pop)

void InitGpus()
{
	GpuCnt = 0;
	int gcnt = 0;
	cudaGetDeviceCount(&gcnt);
	if (gcnt > MAX_GPU_CNT)
		gcnt = MAX_GPU_CNT;

//	gcnt = 1; //dbg
	if (!gcnt)
		return;

	int drv, rt;
	cudaRuntimeGetVersion(&rt);
	cudaDriverGetVersion(&drv);
	char drvver[100];
	sprintf(drvver, "%d.%d/%d.%d", drv / 1000, (drv % 100) / 10, rt / 1000, (rt % 100) / 10);

	printf("CUDA devices: %d, CUDA driver/runtime: %s\r\n", gcnt, drvver);
	cudaError_t cudaStatus;
	for (int i = 0; i < gcnt; i++)
	{
		cudaStatus = cudaSetDevice(i);
		if (cudaStatus != cudaSuccess)
		{
			printf("cudaSetDevice for gpu %d failed!\r\n", i);
			continue;
		}

		if (!gGPUs_Mask[i])
			continue;

		cudaDeviceProp deviceProp;
		cudaGetDeviceProperties(&deviceProp, i);
		printf("GPU %d: %s, %.2f GB, %d CUs, cap %d.%d, PCI %d, L2 size: %d KB\r\n", i, deviceProp.name, ((float)(deviceProp.totalGlobalMem / (1024 * 1024))) / 1024.0f, deviceProp.multiProcessorCount, deviceProp.major, deviceProp.minor, deviceProp.pciBusID, deviceProp.l2CacheSize / 1024);
		
		if (deviceProp.major < 6)
		{
			printf("GPU %d - not supported, skip\r\n", i);
			continue;
		}

		cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);

		GpuKangs[GpuCnt] = new RCGpuKang();
		GpuKangs[GpuCnt]->CudaIndex = i;
		GpuKangs[GpuCnt]->persistingL2CacheMaxSize = deviceProp.persistingL2CacheMaxSize;
		GpuKangs[GpuCnt]->mpCnt = deviceProp.multiProcessorCount;
		GpuKangs[GpuCnt]->IsOldGpu = deviceProp.l2CacheSize < 16 * 1024 * 1024;
		GpuCnt++;
	}
	printf("Total GPUs for work: %d\r\n", GpuCnt);
}
#ifdef _WIN32
u32 __stdcall kang_thr_proc(void* data)
{
	RCGpuKang* Kang = (RCGpuKang*)data;
	Kang->Execute();
	InterlockedDecrement(&ThrCnt);
	return 0;
}
#else
void* kang_thr_proc(void* data)
{
	RCGpuKang* Kang = (RCGpuKang*)data;
	Kang->Execute();
	__sync_fetch_and_sub(&ThrCnt, 1);
	return 0;
}
#endif
void AddPointsToList(u32* data, int pnt_cnt, u64 ops_cnt)
{
	csAddPoints.Enter();
	if (PntIndex + pnt_cnt >= MAX_CNT_LIST)
	{
		csAddPoints.Leave();
		printf("DPs buffer overflow, some points lost, increase DP value!\r\n");
		return;
	}
	memcpy(pPntList + GPU_DP_SIZE * PntIndex, data, pnt_cnt * GPU_DP_SIZE);
	PntIndex += pnt_cnt;
	PntTotalOps += ops_cnt;
	csAddPoints.Leave();
}

bool Collision_SOTA(EcPoint& pnt, EcInt t, int TameType, EcInt w, int WildType, bool IsNeg)
{
	if (IsNeg)
		t.Neg();
	if (TameType == TAME)
	{
		gPrivKey = t;
		gPrivKey.Sub(w);
		EcInt sv = gPrivKey;
		gPrivKey.Add(Int_HalfRange);
		EcPoint P = ec.MultiplyG(gPrivKey);
		if (P.IsEqual(pnt))
			return true;
		gPrivKey = sv;
		gPrivKey.Neg();
		gPrivKey.Add(Int_HalfRange);
		P = ec.MultiplyG(gPrivKey);
		return P.IsEqual(pnt);
	}
	else
	{
		gPrivKey = t;
		gPrivKey.Sub(w);
		if (gPrivKey.data[4] >> 63)
			gPrivKey.Neg();
		gPrivKey.ShiftRight(1);
		EcInt sv = gPrivKey;
		gPrivKey.Add(Int_HalfRange);
		EcPoint P = ec.MultiplyG(gPrivKey);
		if (P.IsEqual(pnt))
			return true;
		gPrivKey = sv;
		gPrivKey.Neg();
		gPrivKey.Add(Int_HalfRange);
		P = ec.MultiplyG(gPrivKey);
		return P.IsEqual(pnt);
	}
}

void AddCheckpointsToList(u8 *pPntList2, int cnt)
{
	csCheckpoints.Enter();
	gPoolCheckpoints.reserve(gPoolCheckpoints.size() + cnt);
	for (int i = 0; i < cnt; ++i)
	{
		u8 *p = pPntList2 + i * GPU_DP_SIZE;
		char buf[105];
		for (int j = 0; j < 12; ++j)
			sprintf(buf + j * 2, "%02x", p[11 - j]);
		buf[24] = ' ';
		u8 d[24];
		for (int j = 0; j < 24; ++j)
			d[j] = p[16 + j];
		if ((d[23] & 0xF0) == 0xF0)
		{
			u8 result[24];
			int borrow = 0;
			for (int k = 0; k < 24; ++k)
			{
				int sub = 0xFF - d[k] - borrow;
				if (sub < 0)
				{
					sub += 0x100;
					borrow = 1;
				}
				else
				{
					borrow = 0;
				}
				result[k] = sub;
			}
			borrow = 1;
			for (int k = 0; k < 24; ++k)
			{
				int sum = result[k] + borrow;
				result[k] = sum & 0xFF;
				borrow = (sum > 0xFF) ? 1 : 0;
			}
			for (int k = 0; k < 24; ++k)
				d[k] = result[k];
		}
		for (int j = 0; j < 8; ++j)
			sprintf(buf + 25 + j * 2, "00");
		for (int j = 0; j < 24; ++j)
			sprintf(buf + 41 + j * 2, "%02x", d[23 - j]);
		int type = p[40];
		snprintf(buf + 25 + 64, 16, " TYPE:%d", type);
		gPoolCheckpoints.emplace_back(buf);
	}
	csCheckpoints.Leave();
}

void GenerateCheckpointFileName()
{
	time_t now = time(nullptr);
	struct tm *t = localtime(&now);
	char datebuf[9];
	snprintf(datebuf, sizeof(datebuf), "%02d-%02d-%02d",
			 t->tm_mday, t->tm_mon + 1, (t->tm_year + 1900) % 100);

	snprintf(gCheckpointFileName, sizeof(gCheckpointFileName),
			 "CHECKPOINTS.%s.%s.%s.%s.TXT",
			 datebuf,
			 gClientID.c_str(),
			 gMachineIdHash4.c_str(),
			 gParamsHash4.c_str());
}

void SaveInitialParamsToFile()
{
	std::ifstream check(gCheckpointFileName);
	if (check.good())
		return;
	FILE *f = fopen(gCheckpointFileName, "a");
	if (f)
	{
		bool wrote = fprintf(f, "%s\n", gRawParams.c_str()) >= 0;
		int ok = fflush(f);
#ifdef _WIN32
		if (ok == 0)
			ok = _commit(_fileno(f));
#else
		if (ok == 0)
			ok = fsync(fileno(f));
#endif
		fclose(f);

		if (wrote && ok == 0)
		{
			csCheckpoints.Enter();
			QueueRemoteLine(gCheckpointFileName, gRawParams);
			TryFlushRemoteQueue();
			csCheckpoints.Leave();
		}
	}
	else
	{
		printf("Error: cannot create initial checkpoint file.\n");
	}
}

void SaveCheckpointToFile()
{
	csCheckpoints.Enter();
	TryFlushRemoteQueue();

	size_t total = gPoolCheckpoints.size();
	if (gSavedLines >= total)
	{
		csCheckpoints.Leave();
		return;
	}

	FILE *f = fopen(gCheckpointFileName, "a");
	if (!f)
	{
		csCheckpoints.Leave();
		printf("Error: cannot open checkpoint file for appending.\n");
		return;
	}

	const size_t oldSaved = gSavedLines;
	size_t i = gSavedLines;

	std::vector<std::string> writtenLines;
	writtenLines.reserve(total - i);
	for (; i < total; ++i)
	{
		const std::string &line = gPoolCheckpoints[i];
		if (!line.empty())
		{
			size_t w = fwrite(line.data(), 1, line.size(), f);
			if (w != line.size())
				break;
		}
		if (fputc('\n', f) == EOF)
			break;
		writtenLines.push_back(line);
	}

	int ok = fflush(f);
#ifdef _WIN32
	if (ok == 0)
		ok = _commit(_fileno(f));
#else
	if (ok == 0)
		ok = fsync(fileno(f));
#endif
	fclose(f);

	if (ok == 0 && !writtenLines.empty())
	{
		for (const std::string &line : writtenLines)
			QueueRemoteLine(gCheckpointFileName, line);
	}

	if (ok == 0 && i == total)
	{
		gSavedLines = 0;
		gPoolCheckpoints.clear();
		gPoolCheckpoints.shrink_to_fit();
	}
	else
	{
		gSavedLines = i;
		if (i == oldSaved)
			printf("Error: failed to write checkpoint lines to file.\n");
		else
			printf("Warning: partial write (%zu/%zu new lines).\n", i - oldSaved, total - oldSaved);
	}

	if (ok == 0 && !writtenLines.empty())
		TryFlushRemoteQueue();

	csCheckpoints.Leave();
}

void CheckNewPoints()
{
	csAddPoints.Enter();
	if (!PntIndex)
	{
		csAddPoints.Leave();
		return;
	}

	int cnt = PntIndex;
	memcpy(pPntList2, pPntList, GPU_DP_SIZE * cnt);
	PntIndex = 0;
	csAddPoints.Leave();

	if (!gPoolAddress.empty() && !gSaveCheckpoints)
	{
		printf("error: --pool-address requires checkpoint saving mode (-nodeID)\n");
		return;
	}

	if (gSaveCheckpoints)
	{
		AddCheckpointsToList(pPntList2, cnt);
	}

	for (int i = 0; i < cnt; i++)
	{
		DBRec nrec;
		u8* p = pPntList2 + i * GPU_DP_SIZE;
		memcpy(nrec.x, p, 12);
		memcpy(nrec.d, p + 16, 22);
		nrec.type = gGenMode ? TAME : p[40];

		DBRec* pref = (DBRec*)db.FindOrAddDataBlock((u8*)&nrec);
		if (gGenMode)
			continue;
		if (pref)
		{
			//in db we dont store first 3 bytes so restore them
			DBRec tmp_pref;
			memcpy(&tmp_pref, &nrec, 3);
			memcpy(((u8*)&tmp_pref) + 3, pref, sizeof(DBRec) - 3);
			pref = &tmp_pref;

			if (pref->type == nrec.type)
			{
				if (pref->type == TAME)
					continue;

				//if it's wild, we can find the key from the same type if distances are different
				if (*(u64*)pref->d == *(u64*)nrec.d)
					continue;
				//else
				//	ToLog("key found by same wild");
			}

			EcInt w, t;
			int TameType, WildType;
			if (pref->type != TAME)
			{
				memcpy(w.data, pref->d, sizeof(pref->d));
				if (pref->d[21] == 0xFF) memset(((u8*)w.data) + 22, 0xFF, 18);
				memcpy(t.data, nrec.d, sizeof(nrec.d));
				if (nrec.d[21] == 0xFF) memset(((u8*)t.data) + 22, 0xFF, 18);
				TameType = nrec.type;
				WildType = pref->type;
			}
			else
			{
				memcpy(w.data, nrec.d, sizeof(nrec.d));
				if (nrec.d[21] == 0xFF) memset(((u8*)w.data) + 22, 0xFF, 18);
				memcpy(t.data, pref->d, sizeof(pref->d));
				if (pref->d[21] == 0xFF) memset(((u8*)t.data) + 22, 0xFF, 18);
				TameType = TAME;
				WildType = nrec.type;
			}

			bool res = Collision_SOTA(gPntToSolve, t, TameType, w, WildType, false) || Collision_SOTA(gPntToSolve, t, TameType, w, WildType, true);
			if (!res)
			{
				bool w12 = ((pref->type == WILD1) && (nrec.type == WILD2)) || ((pref->type == WILD2) && (nrec.type == WILD1));
				if (w12) //in rare cases WILD and WILD2 can collide in mirror, in this case there is no way to find K
					;// ToLog("W1 and W2 collides in mirror");
				else
				{
					printf("Collision Error\r\n");
					gTotalErrors++;
				}
				continue;
			}
			gSolved = true;
			break;
		}
	}
}

void ShowStats(u64 tm_start, double exp_ops, double dp_val)
{
#ifdef DEBUG_MODE
	for (int i = 0; i <= MD_LEN; i++)
	{
		u64 val = 0;
		for (int j = 0; j < GpuCnt; j++)
		{
			val += GpuKangs[j]->dbg[i];
		}
		if (val)
			printf("Loop size %d: %llu\r\n", i, val);
	}
#endif

	if (gSaveCheckpoints)
	{
		time_t now = time(nullptr);
		struct tm *tmNow = localtime(&now);
		int day = tmNow->tm_mday;
		if (day != gLastCheckpointDay)
		{
			gLastCheckpointDay = day;
			GenerateCheckpointFileName();
			SaveInitialParamsToFile();
		}
	}
	if (gSaveCheckpoints)
	{
		SaveCheckpointToFile();
	}

	int speed = GpuKangs[0]->GetStatsSpeed();
	for (int i = 1; i < GpuCnt; i++)
		speed += GpuKangs[i]->GetStatsSpeed();

	u64 est_dps_cnt = (u64)(exp_ops / dp_val);
	u64 exp_sec = 0xFFFFFFFFFFFFFFFFull;
	if (speed)
		exp_sec = (u64)((exp_ops / 1000000) / speed); //in sec
	u64 exp_days = exp_sec / (3600 * 24);
	u64 exp_hours = (exp_sec % (3600 * 24)) / 3600;
	u64 exp_min = (exp_sec % 3600) / 60;

	u64 exp_years = exp_days / 365;
	u64 rem_days = exp_days % 365;

	u64 elapsed_sec = (GetTickCount64() - tm_start) / 1000;
	u64 days = elapsed_sec / 86400;
	u64 hours = (elapsed_sec % 86400) / 3600;
	u64 min = (elapsed_sec % 3600) / 60;
	u64 sec = elapsed_sec % 60;

	const char *mode_str = gGenMode	 ? "GEN: "
						   : IsBench ? "BENCH: "
									 : "MAIN: ";

	printf("\n%s\n", mode_str);
	printf("%-8s%d MKeys/s, Err: %d\n", "Speed:", speed, gTotalErrors);
	printf("%-8s%llu / %llu\n", "DPs:", db.GetBlockCnt(), est_dps_cnt);
	printf("%-8s%llud:%02lluh:%02llum:%02llus / %lluy:%llud:%02lluh:%02llum\n", "Time:",
		   days, hours, min, sec,
		   exp_years, rem_days, exp_hours, exp_min);
}

bool SolvePoint(EcPoint PntToSolve, int Range, int DP, EcInt* pk_res)
{
	if ((Range < 32) || (Range > 180))
	{
		printf("Unsupported Range value (%d)!\r\n", Range);
		return false;
	}
	if ((DP < 14) || (DP > 60)) 
	{
		printf("Unsupported DP value (%d)!\r\n", DP);
		return false;
	}

	printf("\r\nSolving point: Range %d bits, DP %d, start...\r\n", Range, DP);
	double ops = 1.15 * pow(2.0, Range / 2.0);
	double dp_val = (double)(1ull << DP);
	double ram = (32 + 4 + 4) * ops / dp_val; //+4 for grow allocation and memory fragmentation
	ram += sizeof(TListRec) * 256 * 256 * 256; //3byte-prefix table
	ram /= (1024 * 1024 * 1024); //GB
	printf("SOTA method, estimated ops: 2^%.3f, RAM for DPs: %.3f GB. DP and GPU overheads not included!\r\n", log2(ops), ram);
	gIsOpsLimit = false;
	double MaxTotalOps = 0.0;
	if (gMax > 0)
	{
		MaxTotalOps = gMax * ops;
		double ram_max = (32 + 4 + 4) * MaxTotalOps / dp_val; //+4 for grow allocation and memory fragmentation
		ram_max += sizeof(TListRec) * 256 * 256 * 256; //3byte-prefix table
		ram_max /= (1024 * 1024 * 1024); //GB
		printf("Max allowed number of ops: 2^%.3f, max RAM for DPs: %.3f GB\r\n", log2(MaxTotalOps), ram_max);
	}

	u64 total_kangs = GpuKangs[0]->CalcKangCnt();
	for (int i = 1; i < GpuCnt; i++)
		total_kangs += GpuKangs[i]->CalcKangCnt();
	double path_single_kang = ops / total_kangs;	
	double DPs_per_kang = path_single_kang / dp_val;
	printf("Estimated DPs per kangaroo: %.3f.%s\r\n", DPs_per_kang, (DPs_per_kang < 5) ? " DP overhead is big, use less DP value if possible!" : "");

	if (!gGenMode && gTamesFileName[0])
	{
		printf("load tames...\r\n");
		if (db.LoadFromFile(gTamesFileName))
		{
			printf("tames loaded\r\n");
			if (db.Header[0] != gRange)
			{
				printf("loaded tames have different range, they cannot be used, clear\r\n");
				db.Clear();
			}
		}
		else
			printf("tames loading failed\r\n");
	}

	SetRndSeed(0); //use same seed to make tames from file compatible
	PntTotalOps = 0;
	PntIndex = 0;
//prepare jumps
	EcInt minjump, t;
	minjump.Set(1);
	minjump.ShiftLeft(Range / 2 + 3);
	for (int i = 0; i < JMP_CNT; i++)
	{
		EcJumps1[i].dist = minjump;
		t.RndMax(minjump);
		EcJumps1[i].dist.Add(t);
		EcJumps1[i].dist.data[0] &= 0xFFFFFFFFFFFFFFFE; //must be even
		EcJumps1[i].p = ec.MultiplyG(EcJumps1[i].dist);
	}

	minjump.Set(1);
	minjump.ShiftLeft(Range - 10); //large jumps for L1S2 loops. Must be almost RANGE_BITS
	for (int i = 0; i < JMP_CNT; i++)
	{
		EcJumps2[i].dist = minjump;
		t.RndMax(minjump);
		EcJumps2[i].dist.Add(t);
		EcJumps2[i].dist.data[0] &= 0xFFFFFFFFFFFFFFFE; //must be even
		EcJumps2[i].p = ec.MultiplyG(EcJumps2[i].dist);
	}

	minjump.Set(1);
	minjump.ShiftLeft(Range - 10 - 2); //large jumps for loops >2
	for (int i = 0; i < JMP_CNT; i++)
	{
		EcJumps3[i].dist = minjump;
		t.RndMax(minjump);
		EcJumps3[i].dist.Add(t);
		EcJumps3[i].dist.data[0] &= 0xFFFFFFFFFFFFFFFE; //must be even
		EcJumps3[i].p = ec.MultiplyG(EcJumps3[i].dist);
	}
	SetRndSeed(GetTickCount64());

	Int_HalfRange.Set(1);
	Int_HalfRange.ShiftLeft(Range - 1);
	Pnt_HalfRange = ec.MultiplyG(Int_HalfRange);
	Pnt_NegHalfRange = Pnt_HalfRange;
	Pnt_NegHalfRange.y.NegModP();
	Int_TameOffset.Set(1);
	Int_TameOffset.ShiftLeft(Range - 1);
	EcInt tt;
	tt.Set(1);
	tt.ShiftLeft(Range - 5); //half of tame range width
	Int_TameOffset.Sub(tt);
	gPntToSolve = PntToSolve;

//prepare GPUs
	for (int i = 0; i < GpuCnt; i++)
		if (!GpuKangs[i]->Prepare(PntToSolve, Range, DP, EcJumps1, EcJumps2, EcJumps3))
		{
			GpuKangs[i]->Failed = true;
			printf("GPU %d Prepare failed\r\n", GpuKangs[i]->CudaIndex);
		}

	u64 tm0 = GetTickCount64();
	printf("GPUs started...\r\n");

#ifdef _WIN32
	HANDLE thr_handles[MAX_GPU_CNT];
#else
	pthread_t thr_handles[MAX_GPU_CNT];
#endif

	u32 ThreadID;
	gSolved = false;
	ThrCnt = GpuCnt;
	for (int i = 0; i < GpuCnt; i++)
	{
#ifdef _WIN32
		thr_handles[i] = (HANDLE)_beginthreadex(NULL, 0, kang_thr_proc, (void*)GpuKangs[i], 0, &ThreadID);
#else
		pthread_create(&thr_handles[i], NULL, kang_thr_proc, (void*)GpuKangs[i]);
#endif
	}

	u64 tm_stats = GetTickCount64();
	while (!gSolved)
	{
		CheckNewPoints();
		Sleep(10);
		if (GetTickCount64() - tm_stats > 10 * 1000)
		{
			ShowStats(tm0, ops, dp_val);
			tm_stats = GetTickCount64();
		}

		if ((MaxTotalOps > 0.0) && (PntTotalOps > MaxTotalOps))
		{
			gIsOpsLimit = true;
			printf("Operations limit reached\r\n");
			break;
		}
	}
	ShowStats(tm0, ops, dp_val);
	printf("Stopping work ...\r\n");
	for (int i = 0; i < GpuCnt; i++)
		GpuKangs[i]->Stop();
	while (ThrCnt)
		Sleep(10);
	for (int i = 0; i < GpuCnt; i++)
	{
#ifdef _WIN32
		CloseHandle(thr_handles[i]);
#else
		pthread_join(thr_handles[i], NULL);
#endif
	}

	if (gIsOpsLimit)
	{
		if (gGenMode)
		{
			printf("saving tames...\r\n");
			db.Header[0] = gRange; 
			if (db.SaveToFile(gTamesFileName))
				printf("tames saved\r\n");
			else
				printf("tames saving failed\r\n");
		}
		db.Clear();
		return false;
	}

	double K = (double)PntTotalOps / pow(2.0, Range / 2.0);
	printf("Point solved, K: %.3f (with DP and GPU overheads)\r\n\r\n", K);
	db.Clear();
	*pk_res = gPrivKey;
	return true;
}

bool ParseCommandLine(int argc, char* argv[])
{
	int ci = 1;
	while (ci < argc)
	{
		char* argument = argv[ci];
		ci++;
		if (strcmp(argument, "-gpu") == 0)
		{
			if (ci >= argc)
			{
				printf("error: missed value after -gpu option\r\n");
				return false;
			}
			char* gpus = argv[ci];
			ci++;
			memset(gGPUs_Mask, 0, sizeof(gGPUs_Mask));
			for (int i = 0; i < (int)strlen(gpus); i++)
			{
				if ((gpus[i] < '0') || (gpus[i] > '9'))
				{
					printf("error: invalid value for -gpu option\r\n");
					return false;
				}
				gGPUs_Mask[gpus[i] - '0'] = 1;
			}
		}
		else
		if (strcmp(argument, "-dp") == 0)
		{
			int val = atoi(argv[ci]);
			ci++;
			if ((val < 14) || (val > 60))
			{
				printf("error: invalid value for -dp option\r\n");
				return false;
			}
			gDP = val;
		}
		else
		if (strcmp(argument, "-range") == 0)
		{
			int val = atoi(argv[ci]);
			ci++;
			if ((val < 32) || (val > 170))
			{
				printf("error: invalid value for -range option\r\n");
				return false;
			}
			gRange = val;
		}
		else
		if (strcmp(argument, "-start") == 0)
		{	
			if (!gStart.SetHexStr(argv[ci]))
			{
				printf("error: invalid value for -start option\r\n");
				return false;
			}
			ci++;
			gStartSet = true;
		}
		else
		if (strcmp(argument, "-pubkey") == 0)
		{
			if (!gPubKey.SetHexStr(argv[ci]))
			{
				printf("error: invalid value for -pubkey option\r\n");
				return false;
			}
			ci++;
		}
		else
		if (strcmp(argument, "-tames") == 0)
		{
			strcpy(gTamesFileName, argv[ci]);
			ci++;
		}
		else
		if (strcmp(argument, "-max") == 0)
		{
			double val = atof(argv[ci]);
			ci++;
			if (val < 0.001)
			{
				printf("error: invalid value for -max option\r\n");
				return false;
			}
			gMax = val;
		}
		
		else if (strcmp(argument, "--pool-address") == 0)
		{
			if (ci >= argc)
			{
				printf("error: missing value after --pool-address option\n");
				return false;
			}
			gPoolAddress = argv[ci++];
			if (gPoolAddress.rfind("https://", 0) != 0)
			{
				printf("error: pool address must start with https://\n");
				return false;
			}
		}
		
		else if (strcmp(argument, "-nodeID") == 0)
		{
			if (ci >= argc)
			{
				printf("error: missing value after -nodeID option\n");
				return false;
			}
			std::string id = argv[ci++];

			if (id.empty() || id[0] != '@')
			{
				printf("error: nodeID must start with '@' (example: @Worker)\n");
				return false;
			}

			if (id.length() > 20)
			{
				printf("error: nodeID length must not exceed 20 characters\n");
				return false;
			}

			for (size_t i = 1; i < id.size(); ++i)
			{
				if (!std::isalnum(static_cast<unsigned char>(id[i])) && id[i] != '_')
				{
					printf("error: nodeID must contain only letters, digits or '_' after '@'\n");
					return false;
				}
			}

			gClientID = id;
			gSaveCheckpoints = true;
		}

		else
		{
			printf("error: unknown option %s\r\n", argument);
			return false;
		}
	}

	if (gSaveCheckpoints)
	{
		printf("Checkpoint saving mode is enabled.\n");
		gRawParams.clear();
		for (int i = 1; i < argc; ++i)
		{
			if (strcmp(argv[i], "-dp") == 0 ||
				strcmp(argv[i], "-range") == 0 ||
				strcmp(argv[i], "-start") == 0 ||
				strcmp(argv[i], "-pubkey") == 0)
			{
				if (!gRawParams.empty())
					gRawParams += " ";
				gRawParams += argv[i];
				if ((i + 1) < argc)
				{
					gRawParams += " ";
					gRawParams += argv[i + 1];
					++i;
				}
			}
		}
	}

	if (!gPubKey.x.IsZero())
		if (!gStartSet || !gRange || !gDP)
		{
			printf("error: you must also specify -dp, -range and -start options\r\n");
			return false;
		}
	if (gTamesFileName[0] && !IsFileExist(gTamesFileName))
	{
		if (gMax == 0.0)
		{
			printf("error: you must also specify -max option to generate tames\r\n");
			return false;
		}
		gGenMode = true;
	}
	if (gSaveCheckpoints && gDP < 18)
	{
		printf("Error: to save checkpoints DP should be more than 18\n");
		return false;
	}
	if (!gPoolAddress.empty())
		printf("Remote pool synchronization enabled: %s\n", gPoolAddress.c_str());
	return true;
}

void InitMachineIdHash()
{
	std::string exePath;
#ifdef _WIN32
	{
		char buf[MAX_PATH];
		DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
		if (len > 0)
			exePath.assign(buf, len);
	}
#else
	{
		char buf[PATH_MAX];
		ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
		if (len > 0)
		{
			buf[len] = '\0';
			exePath.assign(buf);
		}
	}
#endif
	exePath = Norm(exePath);
	std::string gpuFP;
	int gcnt = 0;
	if (cudaGetDeviceCount(&gcnt) == cudaSuccess && gcnt > 0)
	{
		for (int i = 0; i < gcnt; ++i)
		{
			cudaDeviceProp dp{};
			if (cudaGetDeviceProperties(&dp, i) != cudaSuccess)
				continue;
#if defined(CUDART_VERSION) && (CUDART_VERSION >= 10000)
			const unsigned char *u = reinterpret_cast<const unsigned char *>(&dp.uuid);
			char ubuf[33];
			for (int k = 0; k < 16; ++k)
				sprintf(ubuf + k * 2, "%02x", u[k]);
			gpuFP += "|gpu" + std::to_string(i) + "=" + std::string(ubuf, 32);
#else
			gpuFP += "|gpu" + std::to_string(i) + "=" + std::string(dp.name) + ":" + std::to_string(dp.pciBusID) + ":" + std::to_string((unsigned long long)dp.totalGlobalMem);
#endif
		}
	}

	std::string material = Norm(gMachineId) + "|" + exePath + gpuFP;
	const uint32_t mh = FNV1a16(material);
	std::ostringstream oss;
	oss << std::hex << std::nouppercase << std::setw(4) << std::setfill('0') << mh;
	gMachineIdHash4 = oss.str();

	// DEBUG DUMP (remove after check)
	{
		std::string host = Norm(gMachineId);
		// printf("[MachineHash] host: '%s'\n", host.c_str());
		// printf("[MachineHash] exePath: '%s'\n", exePath.c_str());
		// printf("[MachineHash] gpuFP: '%s'\n", gpuFP.c_str());
		// printf("[MachineHash] material: '%s'\n", material.c_str());
		// printf("[MachineHash] hash: %s\n", gMachineIdHash4.c_str());
	}
}

void InitParamsHash()
{
	char hx[65] = {0}, hy[65] = {0}, hs[129] = {0};
	gPubKey.x.GetHexStr(hx);
	gPubKey.y.GetHexStr(hy);
	gStart.GetHexStr(hs);
	const std::string X = Norm(hx);
	const std::string Y = Norm(hy);
	const std::string S = Norm(hs);
	std::string toHash = "dp=" + std::to_string(gDP) + "|range=" + std::to_string(gRange) + "|start=" + S + "|x=" + X + "|y=" + Y;
	const uint32_t ph = FNV1a16(toHash);
	std::ostringstream oss;
	oss << std::hex << std::nouppercase << std::setw(4) << std::setfill('0') << ph;
	gParamsHash4 = oss.str();
}

int main(int argc, char *argv[])
{
#ifdef _DEBUG
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

	printf("********************************************************************************\r\n");
	printf("*                                                                              *\r\n");
	printf("*                         Kangaroo v%-4s(c) 2025                               *\r\n", gSoftVersion.c_str());
	printf("*                                POOL MODE                                     *\r\n");
	printf("*                                                                              *\r\n");
	printf("********************************************************************************\r\n\r\n");

	// printf("This software is free and open-source: https://github.com/RetiredC\r\n");
	// printf("It demonstrates fast GPU implementation of SOTA Kangaroo method for solving ECDLP\r\n");

#ifdef _WIN32
	printf("Windows version\r\n");
#else
	printf("Linux version\r\n");
#endif

#ifdef DEBUG_MODE
	printf("DEBUG MODE\r\n\r\n");
#endif

	InitEc();
	gDP = 0;
	gRange = 0;
	gStartSet = false;
	gTamesFileName[0] = 0;
	gMax = 0.0;
	gGenMode = false;
	gIsOpsLimit = false;
	memset(gGPUs_Mask, 1, sizeof(gGPUs_Mask));
	if (!ParseCommandLine(argc, argv))
		return 0;

	InitGpus();

	if (!GpuCnt)
	{
		printf("No supported GPUs detected, exit\r\n");
		return 0;
	}

	const char *name = getenv("COMPUTERNAME");
	if (!name)
		name = getenv("HOSTNAME");
	gMachineId = name ? name : "";
	InitMachineIdHash();
	InitParamsHash();

	pPntList = (u8*)malloc(MAX_CNT_LIST * GPU_DP_SIZE);
	pPntList2 = (u8*)malloc(MAX_CNT_LIST * GPU_DP_SIZE);
	TotalOps = 0;
	TotalSolved = 0;
	gTotalErrors = 0;
	IsBench = gPubKey.x.IsZero();

	if (!IsBench && !gGenMode)
	{
		printf("\r\nMAIN MODE\r\n\r\n");
		EcPoint PntToSolve, PntOfs;
		EcInt pk, pk_found;

		PntToSolve = gPubKey;
		if (!gStart.IsZero())
		{
			PntOfs = ec.MultiplyG(gStart);
			PntOfs.y.NegModP();
			PntToSolve = ec.AddPoints(PntToSolve, PntOfs);
		}

		char sx[100], sy[100];
		gPubKey.x.GetHexStr(sx);
		gPubKey.y.GetHexStr(sy);
		printf("Solving public key\r\nX: %s\r\nY: %s\r\n", sx, sy);
		gStart.GetHexStr(sx);
		printf("Offset: %s\r\n", sx);

		if (!SolvePoint(PntToSolve, gRange, gDP, &pk_found))
		{
			if (!gIsOpsLimit)
				printf("FATAL ERROR: SolvePoint failed\r\n");
			goto label_end;
		}
		pk_found.AddModP(gStart);
		EcPoint tmp = ec.MultiplyG(pk_found);
		if (!tmp.IsEqual(gPubKey))
		{
			printf("FATAL ERROR: SolvePoint found incorrect key\r\n");
			goto label_end;
		}
		//happy end
		char s[100];
		pk_found.GetHexStr(s);
		printf("\r\nPRIVATE KEY: %s\r\n\r\n", s);
		FILE* fp = fopen("RESULTS.TXT", "a");
		if (fp)
		{
			fprintf(fp, "PRIVATE KEY: %s\n", s);
			fclose(fp);
		}
		else //we cannot save the key, show error and wait forever so the key is displayed
		{
			printf("WARNING: Cannot save the key to RESULTS.TXT!\r\n");
			while (1)
				Sleep(100);
		}
	}
	else
	{
		if (gGenMode)
			printf("\r\nTAMES GENERATION MODE\r\n");
		else
			printf("\r\nBENCHMARK MODE\r\n");
		//solve points, show K
		while (1)
		{
			EcInt pk, pk_found;
			EcPoint PntToSolve;

			if (!gRange)
				gRange = 78;
			if (!gDP)
				gDP = 16;

			//generate random pk
			pk.RndBits(gRange);
			PntToSolve = ec.MultiplyG(pk);

			if (!SolvePoint(PntToSolve, gRange, gDP, &pk_found))
			{
				if (!gIsOpsLimit)
					printf("FATAL ERROR: SolvePoint failed\r\n");
				break;
			}
			if (!pk_found.IsEqual(pk))
			{
				printf("FATAL ERROR: Found key is wrong!\r\n");
				break;
			}
			TotalOps += PntTotalOps;
			TotalSolved++;
			u64 ops_per_pnt = TotalOps / TotalSolved;
			double K = (double)ops_per_pnt / pow(2.0, gRange / 2.0);
			printf("Points solved: %d, average K: %.3f (with DP and GPU overheads)\r\n", TotalSolved, K);
			//if (TotalSolved >= 100) break; //dbg
		}
	}
label_end:
	for (int i = 0; i < GpuCnt; i++)
		delete GpuKangs[i];
	DeInitEc();
	free(pPntList2);
	free(pPntList);
}

