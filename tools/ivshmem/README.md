# AppSandbox ivshmem driver (Windows guest, macOS host transport)

Our own KMDF driver (no third-party code) that maps the QEMU `ivshmem-plain` PCI BAR2 into
user space so the Windows guest and the macOS host share a memory region (frame buffers +
control). The macOS host mmaps the same `memory-backend-file`; the guest reaches it through
this driver. The patched QEMU that provides the device is vendored at `../../vendor/qemu-ivshmem/`.

## Files
- `AppSandboxSHM.h`  — shared IOCTL/GUID/struct contract (driver + user-mode clients).
- `AppSandboxSHM.c`  — the KMDF function driver (binds VEN_1AF4&DEV_1110, maps BAR2 to user VA).
- `AppSandboxSHM.inf` — install info; SUBSYS-qualified match outranks the inbox RAM-controller driver.

## Build (dev VM: VS2022 + WDK + Windows SDK, ARM64 target)
A KMDF driver builds cleanly via an MSBuild driver `.vcxproj` (WDK project system). Either:
- create a "Kernel Mode Driver, Empty (KMDF)" project, add the 3 files, set target ARM64, or
- build from the EWDK/`VsDevCmd` command line linking the KMDF stub + `WdfDriverEntry`.
Produces `AppSandboxSHM.sys`. (Finalize the exact project once the WDK layout is confirmed; set
`KmdfLibraryVersion` in the INF to the installed WDK's KMDF version.)

## Test-sign + enable test signing (test mode is the design; Secure Boot is OFF in our firmware)
1. Create a test code-signing cert and trust it on the VM:
   - `New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=AppSandbox Test" -CertStoreLocation Cert:\LocalMachine\My`
   - export its public cert; import into `Cert:\LocalMachine\Root` AND `...\TrustedPublisher`.
2. `inf2cat /driver:. /os:10_NI_ARM64` (or appropriate) → `AppSandboxSHM.cat`.
3. `signtool sign /v /fd sha256 /s My /n "AppSandbox Test" AppSandboxSHM.sys AppSandboxSHM.cat`.
4. **Enable test signing** (per project requirement — test-signed driver won't load otherwise):
   `bcdedit /set testsigning on`  → **reboot**.
5. Install: `pnputil /add-driver AppSandboxSHM.inf /install` (rebinds the device from the inbox
   "PCI standard RAM Controller" to ours). Confirm in Device Manager: "AppSandbox ivshmem
   Shared Memory", and `info pci` on the host shows BAR2 now decode-enabled (mapped).

## User-mode use (test app / agent / VDD)
- Default BAR size **128 MiB** (power of 2; sparse backing → only touched pages cost RAM): covers 4K
  double‑buffer + the channel rings (the display stream ring is 32 MiB, see `asb_shm_layout.h`, enough
  for two 1440p or one 4K frame in flight). Driver picks the largest memory BAR.
- `SetupDiGetClassDevs(&GUID_DEVINTERFACE_ASB_IVSHMEM, ...)` → open the device path.
- `DeviceIoControl(h, IOCTL_ASB_IVSHMEM_MAP, NULL,0, &out,sizeof(out), ...)` → `out.userVa`
  is a pointer to the shared region (`out.size` bytes). Read/write directly.
- The macOS host (`tools/shm-test/shm_host.c`, adapted to the ivshmem backing file) mmaps the
  same bytes. Same layout/protocol as the shared-memory POC (per-page header + control fields).
