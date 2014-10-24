#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <stdio.h>

int log_2(int x) {
	return (x > 1) ? log_2(x / 2) + 1 : 0;
}

int main (int argc, char ** argv) {
	int result;
	FILE * f = fopen(argv[1], "w");
#ifdef _WIN32
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	result = si.dwPageSize;
#else
	result = sysconf(_SC_PAGESIZE);
#endif
	if (result == -1) {
		return 1;
	}
	result = log_2(result);
	fprintf(f, "%d\n", result);
	fclose(f);
	return 0;
}