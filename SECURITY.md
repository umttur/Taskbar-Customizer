# Security

Taskbar Customizer uses Windows APIs to customize the taskbar's appearance.

## Application behavior

- The taskbar is verified to belong to Explorer. The application's DLL is loaded through a temporary `WH_CALLWNDPROC` hook.
- The DLL targets only the taskbar's XAML background; blur is not applied to text or icons.
- The application does not access the network, collect telemetry or credentials, or install services or drivers.
- Administrator privileges are not requested; the build script does not download packages.
- Settings are stored locally in `%LOCALAPPDATA%\TaskbarCustomizer\settings.json`.
- **Start with Windows** adds the `TaskbarCustomizer` value under the current user's `HKCU\Software\Microsoft\Windows\CurrentVersion\Run` key only when the user enables it. Disabling the option removes this value.

## DLL loading and appearance restoration

The DLL is loaded using an absolute path from the same folder as the EXE; no
additional digital signature verification is performed. A local control channel
transfers settings and status between the EXE and DLL; it does not authenticate
processes that access the channel.

Disable, Exit, or loss of the EXE connection triggers an attempt to restore the
previous appearance. To keep callbacks valid, the DLL remains loaded until
Explorer exits. To unload it completely or switch to a new version, close the
application, save your work, and restart Windows Explorer. Windows updates may
change the taskbar's internal structure; DLL errors can affect Explorer's operation.

## Reporting security issues

If you encounter a security issue or a security software alert, include the
application version, affected file name, SHA-256 hash, and report details.
Reports are assessed for the specific file and version. Do not include personal
information or confidential data in public reports.
