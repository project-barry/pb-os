#!/usr/bin/env python3
# Re-enable the SystemServer services Lepton comments out that normal apps
# and Play services need (clipboard, restrictions, biometric + auth, NSD,
# cross-profile apps, hardware properties): their managers use
# getServiceOrThrow, so getSystemService() returns null and apps crash.
import sys
p = sys.argv[1]
s = open(p).read()
anchor = '    const-string v0, "AppServiceManager"\n'
assert s.count(anchor) == 1, "anchor"
SSM = "Lcom/android/server/SystemServer;->mSystemServiceManager:Lcom/android/server/SystemServiceManager;"
START = "Lcom/android/server/SystemServiceManager;->startService(Ljava/lang/Class;)Lcom/android/server/SystemService;"
block = "    # pbos: restored services\n"
for cls in ("Lcom/android/server/clipboard/ClipboardService;",
            "Lcom/android/server/restrictions/RestrictionsManagerService;",
            # BiometricService before AuthService, which binds to it at start
            # (stock Android only starts it with fingerprint/face hardware).
            "Lcom/android/server/biometrics/BiometricService;",
            "Lcom/android/server/biometrics/AuthService;",
            "Lcom/android/server/pm/CrossProfileAppsService;"):
    block += f"""    :pbos_try_{cls.split('/')[-1][:-1]}_start
    iget-object v0, v2, {SSM}
    const-class v1, {cls}
    invoke-virtual {{v0, v1}}, {START}
    :pbos_try_{cls.split('/')[-1][:-1]}_end
    .catchall {{:pbos_try_{cls.split('/')[-1][:-1]}_start .. :pbos_try_{cls.split('/')[-1][:-1]}_end}} :pbos_catch_{cls.split('/')[-1][:-1]}
    goto :pbos_done_{cls.split('/')[-1][:-1]}
    :pbos_catch_{cls.split('/')[-1][:-1]}
    move-exception v0
    :pbos_done_{cls.split('/')[-1][:-1]}

"""
block += """    :pbos_try_nsd_start
    iget-object v0, v2, Lcom/android/server/SystemServer;->mSystemContext:Landroid/content/Context;
    invoke-static {v0}, Lcom/android/server/NsdService;->create(Landroid/content/Context;)Lcom/android/server/NsdService;
    move-result-object v1
    const-string v0, "servicediscovery"
    invoke-static {v0, v1}, Landroid/os/ServiceManager;->addService(Ljava/lang/String;Landroid/os/IBinder;)V
    :pbos_try_nsd_end
    .catchall {:pbos_try_nsd_start .. :pbos_try_nsd_end} :pbos_catch_nsd
    goto :pbos_done_nsd
    :pbos_catch_nsd
    move-exception v0
    :pbos_done_nsd

    :pbos_try_hwprops_start
    new-instance v1, Lcom/android/server/HardwarePropertiesManagerService;
    iget-object v0, v2, Lcom/android/server/SystemServer;->mSystemContext:Landroid/content/Context;
    invoke-direct {v1, v0}, Lcom/android/server/HardwarePropertiesManagerService;-><init>(Landroid/content/Context;)V
    const-string v0, "hardware_properties"
    invoke-static {v0, v1}, Landroid/os/ServiceManager;->addService(Ljava/lang/String;Landroid/os/IBinder;)V
    :pbos_try_hwprops_end
    .catchall {:pbos_try_hwprops_start .. :pbos_try_hwprops_end} :pbos_catch_hwprops
    goto :pbos_done_hwprops
    :pbos_catch_hwprops
    move-exception v0
    :pbos_done_hwprops

"""
if "# pbos: restored services" not in s:
    s = s.replace(anchor, block + anchor)
open(p, "w").write(s)
