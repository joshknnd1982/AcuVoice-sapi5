/* av_pause.c -- which way round does set_pausation take its arguments?
 * get_pausation(which) is unambiguous, so it is used to read back what each candidate
 * ordering actually wrote.
 */
#include <windows.h>
#include <stdio.h>

typedef int(__stdcall *fn2)(int, int);
typedef int(__stdcall *fn1)(int);

int main(int argc, char **argv)
{
    HMODULE dll = LoadLibraryA(argc > 1 ? argv[1] : "engine\\Lib\\avcore.dll");
    fn2 set_pausation;
    fn1 get_pausation;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!dll) { printf("load failed %lu\n", GetLastError()); return 1; }
    set_pausation = (fn2)GetProcAddress(dll, "_set_pausation@8");
    get_pausation = (fn1)GetProcAddress(dll, "_get_pausation@4");

    printf("start:            ");
    for (i = 1; i <= 4; i++) printf("P%d=%d ", i, get_pausation(i));
    printf("\n");

    printf("set_pausation(1, 1234) [which, value]:  rc=%d -> ", set_pausation(1, 1234));
    for (i = 1; i <= 4; i++) printf("P%d=%d ", i, get_pausation(i));
    printf("\n");

    printf("set_pausation(1234, 1) [value, which]:  rc=%d -> ", set_pausation(1234, 1));
    for (i = 1; i <= 4; i++) printf("P%d=%d ", i, get_pausation(i));
    printf("\n");

    printf("restore 650:                            rc=%d -> ", set_pausation(650, 1));
    for (i = 1; i <= 4; i++) printf("P%d=%d ", i, get_pausation(i));
    printf("\n");
    return 0;
}
