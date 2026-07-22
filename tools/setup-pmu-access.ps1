# setup-pmu-access.ps1
#
# Prepares the current account for NON-ELEVATED hardware-counter (PMU) profiling with nexus/bench.
# It grants two permissions Microsoft documents as prerequisites for non-admin ETW profiling:
#   1. membership in the built-in "Performance Log Users" group (may start/stop/control ETW sessions), and
#   2. the "Profile system performance" user right (SeSystemProfilePrivilege), for system-wide profiling.
#
# These are necessary but not always sufficient.
# A PMU-backed SystemTraceProvider session can additionally require ACLs on the logger-session and
# System-Trace-Provider GUIDs; those depend on how the profiler creates its session and are NOT set here.
# The legacy NT Kernel Logger stays administrator-only regardless of this script.
# So read this as "grants the standard prerequisites", not "guarantees unelevated PMU access".
#
# It changes exactly two things and nothing else: membership in Performance Log Users, and the
# SeSystemProfilePrivilege assignment for this one account (granted directly via the LSA policy API,
# LsaAddAccountRights — additive, so no other account or right is affected).
# On domain-managed machines, Group Policy may later re-overwrite the assigned right.
#
# Run once from an elevated PowerShell, then sign out and back in (the token picks up the group and right at
# logon, so existing processes will not see them):
#
#   powershell.exe -ExecutionPolicy Bypass -File .\setup-pmu-access.ps1
#
# Pass -Account "DOMAIN\user" to configure an account other than the caller.

[CmdletBinding()]
param(
    [string] $Account = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# The well-known SID of the built-in "Performance Log Users" group (language-independent).
$PerformanceLogUsersSid = [System.Security.Principal.SecurityIdentifier]::new("S-1-5-32-559")

# A tiny P/Invoke wrapper over the LSA "account rights" API. This is the primitive that secedit and the
# Local Security Policy UI both ultimately call. Add() grants one named right to one account (idempotent);
# Enumerate() reads the rights currently assigned to an account. Both need administrator rights.
if (-not ("PmuRights" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class PmuRights
{
    [StructLayout(LayoutKind.Sequential)]
    struct LSA_UNICODE_STRING { public ushort Length; public ushort MaximumLength; public IntPtr Buffer; }

    [StructLayout(LayoutKind.Sequential)]
    struct LSA_OBJECT_ATTRIBUTES
    {
        public int Length; public IntPtr RootDirectory; public IntPtr ObjectName;
        public int Attributes; public IntPtr SecurityDescriptor; public IntPtr SecurityQualityOfService;
    }

    [DllImport("advapi32.dll")]
    static extern uint LsaOpenPolicy(IntPtr SystemName, ref LSA_OBJECT_ATTRIBUTES Attr, int Access, out IntPtr Handle);
    [DllImport("advapi32.dll")]
    static extern uint LsaAddAccountRights(IntPtr Handle, byte[] Sid, LSA_UNICODE_STRING[] Rights, int Count);
    [DllImport("advapi32.dll")]
    static extern uint LsaEnumerateAccountRights(IntPtr Handle, byte[] Sid, out IntPtr Rights, out int Count);
    [DllImport("advapi32.dll")]
    static extern uint LsaClose(IntPtr Handle);
    [DllImport("advapi32.dll")]
    static extern uint LsaFreeMemory(IntPtr Buffer);
    [DllImport("advapi32.dll")]
    static extern int LsaNtStatusToWinError(uint Status);

    const int POLICY_CREATE_ACCOUNT = 0x00000010;
    const int POLICY_LOOKUP_NAMES = 0x00000800;
    const uint STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;

    static IntPtr OpenPolicy()
    {
        var attr = new LSA_OBJECT_ATTRIBUTES();
        attr.Length = Marshal.SizeOf(typeof(LSA_OBJECT_ATTRIBUTES));
        IntPtr handle;
        uint status = LsaOpenPolicy(IntPtr.Zero, ref attr, POLICY_CREATE_ACCOUNT | POLICY_LOOKUP_NAMES, out handle);
        if (status != 0) throw new Win32Exception(LsaNtStatusToWinError(status), "LsaOpenPolicy failed");
        return handle;
    }

    static LSA_UNICODE_STRING ToLsaString(string s)
    {
        var lus = new LSA_UNICODE_STRING();
        lus.Buffer = Marshal.StringToHGlobalUni(s);
        lus.Length = (ushort)(s.Length * 2);
        lus.MaximumLength = (ushort)((s.Length + 1) * 2);
        return lus;
    }

    public static void Add(byte[] sid, string right)
    {
        IntPtr policy = OpenPolicy();
        try
        {
            var rights = new LSA_UNICODE_STRING[1];
            rights[0] = ToLsaString(right);
            try
            {
                uint status = LsaAddAccountRights(policy, sid, rights, 1);
                if (status != 0) throw new Win32Exception(LsaNtStatusToWinError(status), "LsaAddAccountRights failed");
            }
            finally { Marshal.FreeHGlobal(rights[0].Buffer); }
        }
        finally { LsaClose(policy); }
    }

    public static string[] Enumerate(byte[] sid)
    {
        IntPtr policy = OpenPolicy();
        try
        {
            IntPtr rightsPtr; int count;
            uint status = LsaEnumerateAccountRights(policy, sid, out rightsPtr, out count);
            if (status == STATUS_OBJECT_NAME_NOT_FOUND) return new string[0];
            if (status != 0) throw new Win32Exception(LsaNtStatusToWinError(status), "LsaEnumerateAccountRights failed");
            try
            {
                var result = new string[count];
                int stride = Marshal.SizeOf(typeof(LSA_UNICODE_STRING));
                for (int i = 0; i < count; i++)
                {
                    IntPtr item = (IntPtr)((long)rightsPtr + i * stride);
                    var lus = (LSA_UNICODE_STRING)Marshal.PtrToStructure(item, typeof(LSA_UNICODE_STRING));
                    result[i] = Marshal.PtrToStringUni(lus.Buffer, lus.Length / 2);
                }
                return result;
            }
            finally { LsaFreeMemory(rightsPtr); }
        }
        finally { LsaClose(policy); }
    }
}
'@
}

# Refuse to run unless elevated — both changes below require administrator rights.
function Assert-Administrator {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [System.Security.Principal.WindowsPrincipal]::new($identity)

    if (-not $principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "Run this script from an elevated PowerShell."
    }
}

# Add the account to Performance Log Users. Identify the group by SID (never the localized/qualified name),
# and let a real failure (unresolvable account, access denied) surface instead of being swallowed.
function Add-ToPerformanceLogUsers([string] $AccountName) {
    try {
        Add-LocalGroupMember -SID $PerformanceLogUsersSid -Member $AccountName -ErrorAction Stop
        Write-Host "Added '$AccountName' to Performance Log Users."
    }
    catch [Microsoft.PowerShell.Commands.MemberExistsException] {
        Write-Host "'$AccountName' is already a member of Performance Log Users."
    }
}

# Confirm both changes actually took effect; throw (do not print success) otherwise.
function Assert-Configured([string] $AccountName, [byte[]] $SidBytes, [string] $SidValue) {
    $inGroup = $false
    foreach ($member in @(Get-LocalGroupMember -SID $PerformanceLogUsersSid)) {
        if ($member.SID.Value -eq $SidValue) { $inGroup = $true; break }
    }
    if (-not $inGroup) { throw "Verification failed: '$AccountName' is not in Performance Log Users." }

    if ([PmuRights]::Enumerate($SidBytes) -notcontains "SeSystemProfilePrivilege") {
        throw "Verification failed: SeSystemProfilePrivilege is not assigned to '$AccountName'."
    }
}

Assert-Administrator

# Resolve the account to a SID once, in both the string and binary forms the calls below need.
$sid = [System.Security.Principal.NTAccount]::new($Account).Translate([System.Security.Principal.SecurityIdentifier])
$sidValue = $sid.Value
$sidBytes = New-Object byte[] ($sid.BinaryLength)
$sid.GetBinaryForm($sidBytes, 0)

Write-Host "Configuring PMU access for '$Account' (SID $sidValue)..."
Add-ToPerformanceLogUsers $Account

Write-Host "Granting SeSystemProfilePrivilege..."
[PmuRights]::Add($sidBytes, "SeSystemProfilePrivilege")

Assert-Configured $Account $sidBytes $sidValue

Write-Host ""
Write-Host "Verified. Sign out and back in before measuring hardware counters unelevated."
Write-Host "Note: a PMU-backed ETW session may still need per-session ACLs (see the header) that this script does not set."
