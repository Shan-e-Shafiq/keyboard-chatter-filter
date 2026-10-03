# Windows service

`keyboard-chatter-filter install` (elevated) registers the service through the Service Control
Manager API. The equivalent manual configuration, for packagers and for reference:

```bat
sc.exe create keyboard-chatter-filter binPath= "\"C:\Program Files\keyboard-chatter-filter\keyboard-chatter-filter.exe\" windows-service" start= auto DisplayName= "Keyboard Chatter Filter"
sc.exe description keyboard-chatter-filter "Removes duplicate keystrokes caused by keyboard switch chatter. Keystrokes are processed in memory only; nothing is logged or transmitted."
sc.exe failure keyboard-chatter-filter reset= 86400 actions= restart/5000/restart/5000/restart/30000
sc.exe failureflag keyboard-chatter-filter 1
sc.exe start keyboard-chatter-filter
```

* Account: LocalSystem (required to start the per-session agents with `WTSQueryUserToken`; the
  agents themselves run as the logged-on user).
* The executable must be in a directory only administrators can modify (`%ProgramFiles%`).
* Configuration directory: `%ProgramData%\keyboard-chatter-filter` with ACL
  `D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)` (SYSTEM/Administrators full control,
  Users read-only).
* Controls accepted: stop, shutdown, session change, parameter change (`sc.exe control
  keyboard-chatter-filter paramchange` reloads the configuration in every session).

Removal: `keyboard-chatter-filter uninstall` or `scripts/uninstall.ps1`.
