
#include "precomp.h"

#define SIZEOF_DEVMODEW_300_W 188
#define SIZEOF_DEVMODEW_400_W 212
#define SIZEOF_DEVMODEW_500_W 220

#define SIZEOF_DEVMODEW_300_A 124
#define SIZEOF_DEVMODEW_400_A 148
#define SIZEOF_DEVMODEW_500_A 156

static void
TestCurrentRegistryAndCdsTest(void)
{
    DEVMODEW Current, Registry, Indexed, After;
    LONG Result;
    DWORD Index;
    BOOL Found = FALSE;

    ZeroMemory(&Current, sizeof(Current));
    Current.dmSize = sizeof(Current);
    ok(EnumDisplaySettingsExW(NULL, ENUM_CURRENT_SETTINGS, &Current, 0),
       "EnumDisplaySettingsExW(ENUM_CURRENT_SETTINGS) failed, error %lu\n", GetLastError());

    ZeroMemory(&Registry, sizeof(Registry));
    Registry.dmSize = sizeof(Registry);
    ok(EnumDisplaySettingsExW(NULL, ENUM_REGISTRY_SETTINGS, &Registry, 0),
       "EnumDisplaySettingsExW(ENUM_REGISTRY_SETTINGS) failed, error %lu\n", GetLastError());

    for (Index = 0; Index < 512; Index++)
    {
        ZeroMemory(&Indexed, sizeof(Indexed));
        Indexed.dmSize = sizeof(Indexed);
        if (!EnumDisplaySettingsExW(NULL, Index, &Indexed, 0))
            break;
        if (Indexed.dmPelsWidth == Current.dmPelsWidth &&
            Indexed.dmPelsHeight == Current.dmPelsHeight &&
            Indexed.dmBitsPerPel == Current.dmBitsPerPel)
        {
            Found = TRUE;
        }
    }
    ok(Found, "The current %lux%lux%lu mode was absent from the indexed list\n",
       Current.dmPelsWidth, Current.dmPelsHeight, Current.dmBitsPerPel);

    Result = ChangeDisplaySettingsExW(NULL, &Current, NULL, CDS_TEST, NULL);
    ok(Result == DISP_CHANGE_SUCCESSFUL, "CDS_TEST for the current mode returned %ld\n", Result);

    ZeroMemory(&After, sizeof(After));
    After.dmSize = sizeof(After);
    ok(EnumDisplaySettingsExW(NULL, ENUM_CURRENT_SETTINGS, &After, 0),
       "The post-CDS_TEST current-mode query failed, error %lu\n", GetLastError());
    ok(After.dmPelsWidth == Current.dmPelsWidth &&
       After.dmPelsHeight == Current.dmPelsHeight &&
       After.dmBitsPerPel == Current.dmBitsPerPel &&
       After.dmDisplayFrequency == Current.dmDisplayFrequency,
       "CDS_TEST changed the current mode\n");
}

START_TEST(EnumDisplaySettings)
{
    DEVMODEW dmW;
    DEVMODEA dmA;
    HDC icDisplay;

    /* TODO: test with a printer driver */

    icDisplay = CreateICW(L"DISPLAY", NULL, NULL, NULL);
    ok(icDisplay != NULL, "\n");

    dmW.dmDriverExtra = 0x7777;

    /* Try ridiculous size */
    dmW.dmSize = 0x8888;
    ok(EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dmW), "\n");
    ok(dmW.dmBitsPerPel == GetDeviceCaps(icDisplay, BITSPIXEL), "%lu, should be %d.\n", dmW.dmBitsPerPel, GetDeviceCaps(icDisplay, BITSPIXEL));
    ok(dmW.dmSize == SIZEOF_DEVMODEW_300_W, "%d\n", dmW.dmSize);
    ok(dmW.dmDriverExtra == 0, "%d\n", dmW.dmDriverExtra);

    /* Try a tiny size */
    dmW.dmSize = 4;
    ok(EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dmW), "\n");
    ok(dmW.dmBitsPerPel == GetDeviceCaps(icDisplay, BITSPIXEL), "%lu, should be %d.\n", dmW.dmBitsPerPel, GetDeviceCaps(icDisplay, BITSPIXEL));
    ok(dmW.dmSize == SIZEOF_DEVMODEW_300_W, "%d\n", dmW.dmSize);

    /* Something in between */
    dmW.dmSize = (SIZEOF_DEVMODEW_300_W + SIZEOF_DEVMODEW_400_W) / 2 ;
    ok(EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dmW), "\n");
    ok(dmW.dmBitsPerPel == GetDeviceCaps(icDisplay, BITSPIXEL), "%lu, should be %d.\n", dmW.dmBitsPerPel, GetDeviceCaps(icDisplay, BITSPIXEL));
    ok(dmW.dmSize == SIZEOF_DEVMODEW_300_W, "%d\n", dmW.dmSize);

    /* WINVER < 0x0400 */
    dmW.dmSize = SIZEOF_DEVMODEW_300_W;
    ok(EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dmW), "\n");
    ok(dmW.dmBitsPerPel == GetDeviceCaps(icDisplay, BITSPIXEL), "%lu, should be %d.\n", dmW.dmBitsPerPel, GetDeviceCaps(icDisplay, BITSPIXEL));
    ok(dmW.dmSize == SIZEOF_DEVMODEW_300_W, "%d\n", dmW.dmSize);

    /* WINVER < 0x0500 */
    dmW.dmSize = SIZEOF_DEVMODEW_400_W;
    ok(EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dmW), "\n");
    ok(dmW.dmBitsPerPel == GetDeviceCaps(icDisplay, BITSPIXEL), "%lu, should be %d.\n", dmW.dmBitsPerPel, GetDeviceCaps(icDisplay, BITSPIXEL));
    ok(dmW.dmSize == SIZEOF_DEVMODEW_300_W, "%d\n", dmW.dmSize);

    /* "Modern" windows */
    dmW.dmSize = SIZEOF_DEVMODEW_500_W;
    ok(EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dmW), "\n");
    ok(dmW.dmBitsPerPel == GetDeviceCaps(icDisplay, BITSPIXEL), "%lu, should be %d.\n", dmW.dmBitsPerPel, GetDeviceCaps(icDisplay, BITSPIXEL));
    ok(dmW.dmSize == SIZEOF_DEVMODEW_300_W, "%d\n", dmW.dmSize);

    DeleteDC(icDisplay);

    icDisplay = CreateICA("DISPLAY", NULL, NULL, NULL);
    ok(icDisplay != NULL, "\n");

    dmA.dmDriverExtra = 0x7777;

    /* Try ridiculous size */
    dmA.dmSize = 0x8888;
    ok(EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dmA), "\n");
    ok(dmA.dmBitsPerPel == GetDeviceCaps(icDisplay, BITSPIXEL), "%lu, should be %d.\n", dmA.dmBitsPerPel, GetDeviceCaps(icDisplay, BITSPIXEL));
    ok(dmA.dmSize == SIZEOF_DEVMODEW_300_A, "%d\n", dmA.dmSize);
    ok(dmA.dmDriverExtra == 0, "%d\n", dmA.dmDriverExtra);

    /* Try a tiny size */
    dmA.dmSize = 4;
    ok(EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dmA), "\n");
    ok(dmA.dmBitsPerPel == GetDeviceCaps(icDisplay, BITSPIXEL), "%lu, should be %d.\n", dmA.dmBitsPerPel, GetDeviceCaps(icDisplay, BITSPIXEL));
    ok(dmA.dmSize == SIZEOF_DEVMODEW_300_A, "%d\n", dmA.dmSize);

    /* Something in between */
    dmA.dmSize = (SIZEOF_DEVMODEW_300_A + SIZEOF_DEVMODEW_400_A) / 2 ;
    ok(EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dmA), "\n");
    ok(dmA.dmBitsPerPel == GetDeviceCaps(icDisplay, BITSPIXEL), "%lu, should be %d.\n", dmA.dmBitsPerPel, GetDeviceCaps(icDisplay, BITSPIXEL));
    ok(dmA.dmSize == SIZEOF_DEVMODEW_300_A, "%d\n", dmA.dmSize);

    /* WINVER < 0x0400 */
    dmA.dmSize = SIZEOF_DEVMODEW_300_A;
    ok(EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dmA), "\n");
    ok(dmA.dmBitsPerPel == GetDeviceCaps(icDisplay, BITSPIXEL), "%lu, should be %d.\n", dmA.dmBitsPerPel, GetDeviceCaps(icDisplay, BITSPIXEL));
    ok(dmA.dmSize == SIZEOF_DEVMODEW_300_A, "%d\n", dmA.dmSize);

    /* WINVER < 0x0500 */
    dmA.dmSize = SIZEOF_DEVMODEW_400_A;
    ok(EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dmA), "\n");
    ok(dmA.dmBitsPerPel == GetDeviceCaps(icDisplay, BITSPIXEL), "%lu, should be %d.\n", dmA.dmBitsPerPel, GetDeviceCaps(icDisplay, BITSPIXEL));
    ok(dmA.dmSize == SIZEOF_DEVMODEW_300_A, "%d\n", dmA.dmSize);

    /* "Modern" windows */
    dmA.dmSize = SIZEOF_DEVMODEW_500_A;
    ok(EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dmA), "\n");
    ok(dmA.dmBitsPerPel == GetDeviceCaps(icDisplay, BITSPIXEL), "%lu, should be %d.\n", dmA.dmBitsPerPel, GetDeviceCaps(icDisplay, BITSPIXEL));
    ok(dmA.dmSize == SIZEOF_DEVMODEW_300_A, "%d\n", dmA.dmSize);

    DeleteDC(icDisplay);

    TestCurrentRegistryAndCdsTest();
}
