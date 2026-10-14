"""Reads or changes one DWORD setting in the NVIDIA driver profile of MonsterHunterWorld.exe (NVAPI DRS,
the store the NVIDIA Control Panel and NVIDIA App write). Settings take effect when the game starts.

usage: python -I nvprofile.py get|set|delete SETTING [VALUE]
  SETTING: a hex id or a name: frl (Max Frame Rate, fps; 0x10835002), prerender (max pre-rendered frames,
           the Control Panel's Low Latency Mode On = 1; 0x007ba09e)
  delete removes the game profile's own value, so the global profile's value applies again.
"""
import ctypes
import struct
import sys

EXE = "MonsterHunterWorld.exe"
NAMES = {"frl": 0x10835002, "prerender": 0x007BA09E}
LOCATIONS = {0: "game profile", 1: "global profile", 2: "base profile", 3: "driver default"}

# NVAPI ids (nvapi_interface.h) and structure sizes (nvapi.h, checked with MSVC x64).
ID = {
    "Initialize": 0x0150E828, "CreateSession": 0x0694D52E, "DestroySession": 0xDAD9CFF8,
    "LoadSettings": 0x375DBD6B, "SaveSettings": 0xFCBC7E14, "FindApplicationByName": 0xEEE566B2,
    "GetSetting": 0x73BF8338, "SetSetting": 0x577DD202, "DeleteProfileSetting": 0xE4A26362,
}
SETTING_SIZE, SETTING_VER = 12320, 0x13020        # NVDRS_SETTING_V1
SETTING_ID, SETTING_TYPE, SETTING_LOCATION, SETTING_CURRENT = 4100, 4104, 4108, 8220
APP_SIZE, APP_VER = 20492, 0x4500C                # NVDRS_APPLICATION_V4

nvapi = ctypes.WinDLL("nvapi64.dll")  # System32 on NVIDIA systems
query = nvapi.nvapi_QueryInterface
query.restype = ctypes.c_void_p
query.argtypes = [ctypes.c_uint32]


def fn(name, *args):
    address = query(ID[name])
    if not address:
        sys.exit(f"NVAPI has no {name}")
    return ctypes.CFUNCTYPE(ctypes.c_int, *args)(address)


def check(status, what):
    if status != 0:
        sys.exit(f"{what} failed ({status})")


def setting_buffer(setting_id=0, value=None):
    buf = ctypes.create_string_buffer(SETTING_SIZE)
    struct.pack_into("<I", buf, 0, SETTING_VER)
    if value is not None:
        struct.pack_into("<I", buf, SETTING_ID, setting_id)
        struct.pack_into("<I", buf, SETTING_TYPE, 0)  # NVDRS_DWORD_TYPE
        struct.pack_into("<I", buf, SETTING_CURRENT, value)
    return buf


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("get", "set", "delete") or (sys.argv[1] == "set" and len(sys.argv) < 4):
        sys.exit(__doc__)
    action = sys.argv[1]
    setting = NAMES.get(sys.argv[2].lower()) or int(sys.argv[2], 0)
    vp = ctypes.c_void_p
    check(fn("Initialize")(), "NvAPI_Initialize")
    session = vp()
    check(fn("CreateSession", ctypes.POINTER(vp))(ctypes.byref(session)), "DRS_CreateSession")
    check(fn("LoadSettings", vp)(session), "DRS_LoadSettings")
    name = (ctypes.c_uint16 * 2048)(*[ord(c) for c in EXE])
    profile = vp()
    app = ctypes.create_string_buffer(APP_SIZE)
    struct.pack_into("<I", app, 0, APP_VER)
    check(fn("FindApplicationByName", vp, ctypes.c_void_p, ctypes.POINTER(vp), ctypes.c_void_p)(
        session, ctypes.addressof(name), ctypes.byref(profile), ctypes.addressof(app)), f"finding the {EXE} profile")
    get = fn("GetSetting", vp, vp, ctypes.c_uint32, ctypes.c_void_p)

    def show(when):
        buf = setting_buffer()
        status = get(session, profile, setting, ctypes.addressof(buf))
        if status != 0:
            print(f"{when}: {setting:#010x} not set anywhere ({status})")
            return
        value, = struct.unpack_from("<I", buf, SETTING_CURRENT)
        location, = struct.unpack_from("<I", buf, SETTING_LOCATION)
        print(f"{when}: {setting:#010x} = {value} (from the {LOCATIONS.get(location, location)})")

    show("before")
    if action == "set":
        buf = setting_buffer(setting, int(sys.argv[3], 0))
        check(fn("SetSetting", vp, vp, ctypes.c_void_p)(session, profile, ctypes.addressof(buf)), "DRS_SetSetting")
    elif action == "delete":
        status = fn("DeleteProfileSetting", vp, vp, ctypes.c_uint32)(session, profile, setting)
        if status != 0:
            print(f"the game profile has no value of its own ({status})")
    if action != "get":
        check(fn("SaveSettings", vp)(session), "DRS_SaveSettings")
        show("after")
    fn("DestroySession", vp)(session)


if __name__ == "__main__":
    main()
