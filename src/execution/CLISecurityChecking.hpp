#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <regex>
#include <string>
#include <vector>

/*
    Looks at code the agent is about to run (execute_code) for what could harm this computer or the user's data,
    so that it runs only after the user says yes. Check() returns what it found; ChatAgent then asks the front end
    (the console in cli_main.cpp, a popup in ChatWindow.hpp) through a ConfirmHandler, and flagged code the user
    does not allow is not run.

    The rules cover both ecosystems, because commands cross languages: a Python script passes shell commands as
    strings, PowerShell runs cmd commands, and WSL or Git Bash run Linux commands on Windows.
    - Windows (cmd, PowerShell): deleting, formatting disks, the registry, accounts, Defender and the firewall,
      backups and event logs, boot settings, scheduled tasks and services.
    - Linux (sh, bash): rm -rf, mkfs, dd, sudo, chmod 777, cron, iptables, curl | sh.
    - Python: shutil.rmtree, os.remove, winreg, exec, the network modules.
    - Any: downloading and running code, network access (data must not leave this machine), stored passwords and
      keys, remote access.

    DANGER: could destroy data or the system, weaken its protection, or let someone else in.
    CAUTION: changes this computer or uses the network; usually recoverable.
    Both ask the user; the level only says how worried to be.

    This is pattern matching, a guard against a careless model, not a sandbox: code written to hide what it does
    (a command built from pieces at run time, an unusual alias) can get past it. Code that runs text as code or
    decodes hidden code is flagged for that reason. Rules err on the side of warning: a false alarm costs the user
    one answer, a miss can cost their files.

    The command rules read cmd and PowerShell code whole but only the string literals of Python code, so Python's
    `del x` is not taken for cmd's `del`; the other rules read all of the code.
*/
class CLISecurityChecking {
public:
    enum class Severity { Danger, Caution };

    struct Finding {
        Severity            severity;
        const char*         ecosystem;  // "Windows", "Linux", "Python" or "Any"
        const char*         harm;       // what the code could do, in plain words
        int                 line;       // first line it was found on, from 1; 0: not about a line (where a file goes)
        std::string         evidence;   // that line, shortened
        std::vector<int>    more_lines; // other lines with the same harm
    };

    struct Report {
        std::vector<Finding> findings;  // DANGER first, one per harm
        /*
            Set when the agent wants to write a file (write_file) rather than run code: its full path. The code
            given with the report is then the file's content.
        */
        std::string          file;
        
        // With `file`: the agent wants to delete that file (delete_file).
        bool                 deletes = false;

        bool Flagged() const { return !findings.empty(); }

        bool AnyDanger() const {
            return std::any_of(findings.begin(), findings.end(),
                               [](const Finding& f) { return f.severity == Severity::Danger; });
        }

        /*
            One finding per entry, for people:
                DANGER   deletes whole folders, or files in every subfolder (Windows)
                         line 2: Remove-Item C:\temp\x -Recurse -Force
        */
        std::string Describe() const {
            std::string text;
            for (const Finding& f : findings) {
                text += std::string(SeverityName(f.severity)) + (f.severity == Severity::Danger ? "   " : "  ") +
                        f.harm;
                if (std::strcmp(f.ecosystem, "Any") != 0) {
                    text += std::string(" (") + f.ecosystem + ")";
                }
                text += "\n         " + (f.line > 0 ? "line " + std::to_string(f.line) + ": " : std::string()) +
                        f.evidence;
                if (!f.more_lines.empty()) {
                    text += " (and " + std::to_string(f.more_lines.size()) + " more line" +
                            (f.more_lines.size() > 1 ? "s" : "") + ")";
                }
                text += "\n";
            }
            return text;
        }

        // The harms on one line, for the model: "deletes files; uses the network: ...".
        std::string Harms() const {
            std::string text;
            for (const Finding& f : findings) {
                text += (text.empty() ? "" : "; ") + std::string(f.harm);
            }
            return text;
        }
    };

    /*
        Asks the user whether flagged code may run, or, when `report.file` is set, whether that file may be written
        with `code` as its content (deleted, when `report.deletes` is set); true goes ahead. 
        Called on the thread that runs the agent, which waits for the
        answer.
    */
    using ConfirmHandler = std::function<bool(const std::string& language, const std::string& code, const Report& report)>;

    static const char* SeverityName(Severity severity) {
        return severity == Severity::Danger ? "DANGER" : "CAUTION";
    }

    /*
        What in `code` could do harm. `language` as execute_code receives it (python, powershell, cmd...); an
        unknown one is read like cmd and PowerShell. Thread-safe. Throws std::regex_error only if a rule in the
        table is malformed, which the harness reports as a tool failure, so the code still does not run.
    */
    static Report Check(const std::string& language, const std::string& code) {
        const bool              python        = IsPython(language);
        const std::vector<Line> code_lines    = LogicalLines(code);
        const std::vector<Line> command_lines = python ? LogicalLines(PythonStrings(code)) : code_lines;
        const std::vector<std::string> physical = PhysicalLines(code);

        Report report;
        for (const CompiledRule& rule : CompiledRules()) {
            const std::vector<Line>& lines = rule.def->scope == Scope::Command ? command_lines : code_lines;
            for (const Line& line : lines) {
                if (Matches(rule, line.text)) {
                    Add(report, *rule.def, line.number, physical);
                }
            }
        }
        std::stable_sort(report.findings.begin(), report.findings.end(), [](const Finding& a, const Finding& b) {
            return a.severity == Severity::Danger && b.severity != Severity::Danger;
        });
        return report;
    }

private:
    enum class Scope {
        Command,     // cmd and PowerShell code, and the strings of Python code
        Everywhere,  // all of the code, in any language
    };

    struct Rule {
        Scope       scope;
        Severity    severity;
        const char* ecosystem;
        const char* harm;
        const char* pattern;           // ECMAScript, case-insensitive, searched in one line
        const char* also = nullptr;    // when set, the same line must match this too
    };

    struct CompiledRule {
        const Rule* def;
        std::regex  pattern;
        std::regex  also;
    };

    struct Line {
        int         number;  // of its first physical line, from 1
        std::string text;
    };

    // Long lines are searched in overlapping pieces: libstdc++'s regex recurses once per character it reads.
    static constexpr size_t kChunkBytes    = 512;
    static constexpr size_t kChunkStep     = 384;
    static constexpr size_t kEvidenceBytes = 120;

    /*
        On the same line as a system folder, these make it a change rather than a read. ">" is any redirection,
        so `dir "C:\Program Files" > list.txt` is flagged too: a false alarm, but a cheap one.
    */
    static constexpr const char* kWriteVerbs = R"~(\b(Set-Content|Add-Content|Out-File|Copy-Item|Move-Item|Rename-Item|New-Item|Remove-Item|Clear-Content|copy|xcopy|robocopy|move|ren|rename|del|erase|rd|rmdir|mklink|icacls|takeown|attrib|rm|cp|mv|tee|ln|truncate|dd|touch|mkdir|chmod|chown)\b|\bsed\s+-i\b|>|\bshutil\.(copy\w*|move|rmtree)\b|\bos\.(remove|unlink|rename|replace|rmdir|makedirs|mkdir|chmod)\b|\.write_(text|bytes)\b|\bopen\s*\(.*['"][wax]b?\+?['"])~";

    /*
        The rules, DANGER first. Findings with the same harm text are merged, so rules for the same harm in
        different ecosystems share it. Patterns use \\+ for path separators: Python source writes "C:\\Windows".
        This its on track now the main problem its that in the model because of the stream the message does not fully appear
        WARNING: So this is important for safety does not try crap stuff for now!
    */
    static const std::vector<Rule>& RuleTable() {
        using S = Scope;
        using V = Severity;
        static const std::vector<Rule> rules = {
            /* ---- DANGER ---- */
            {S::Command, V::Danger, "Linux", "deletes whole folders, or files in every subfolder",
             R"~(\brm\s+(.*\s)?-[a-z]?r[a-z]{0,2}(\s|$)|\brm\b.*\s--recursive\b|--no-preserve-root|\bfind\b.*\s-delete\b|\bfind\b.*-exec\s+rm\b)~"},
            {S::Command, V::Danger, "Windows", "deletes whole folders, or files in every subfolder",
             R"~(\b(rd|rmdir|del|erase)\b.*\s/s\b|\b(Remove-Item|ri|rm|rmdir|rd|del|erase)\b.*\s-r(e(c(u(r(s(e)?)?)?)?)?)?\b|\s-rec\w*\b.*\|\s*(%\s*\{\s*)?(Remove-Item|ri|rm|del|erase)\b)~"},
            {S::Everywhere, V::Danger, "Python", "deletes whole folders, or files in every subfolder",
             R"~(\brmtree\s*\()~"},
            {S::Command, V::Danger, "Any", "deletes or wipes a whole drive, the system or the home folder",
             R"~(\b(rm|Remove-Item|ri|rd|rmdir|del|erase|shred)\b.*\s["']?([a-z]:\\*\*?(\.\*)?|/\*?|~/?\*?|\$HOME/?\*?|%USERPROFILE%\\*\*?|\$env:USERPROFILE\\*\*?|%SystemRoot%|%windir%|\$env:(SystemRoot|windir)|[a-z]:\\+Windows|/(etc|usr|bin|sbin|boot|var|home|lib|root))["']?(\s|$))~"},
            {S::Command, V::Danger, "Windows", "erases or repartitions a disk",
             R"~(\bformat(\.com)?\s+[a-z]:|\b(Format-Volume|Clear-Disk|Initialize-Disk|Remove-Partition|New-Partition|Resize-Partition|Set-Partition|Remove-PhysicalDisk)\b|\bdiskpart\b|\bcipher(\.exe)?\b.*\s/w\b)~"},
            {S::Command, V::Danger, "Linux", "erases or repartitions a disk",
             R"~(\b(mkfs(\.\w+)?|fdisk|sfdisk|cfdisk|gdisk|parted|wipefs|mkswap|blkdiscard)\b|\bdd\b.*\bof=/dev/|>\s*/dev/(sd|hd|vd|xvd|nvme|mmcblk|disk)\w*)~"},
            {S::Everywhere, V::Danger, "Windows", "changes or deletes registry settings",
             R"~(\breg(\.exe)?\s+(add|delete|import|load|unload|restore|copy)\b|\bregedit(\.exe)?\s+/s\b|\bwinreg\.(SetValue|SetValueEx|DeleteKey|DeleteKeyEx|DeleteValue|CreateKey|CreateKeyEx)\b|\b(Set-ItemProperty|New-ItemProperty|Remove-ItemProperty|Rename-ItemProperty|Set-Item|New-Item|Remove-Item|sp|ni|ri)\b.*\b(HKLM|HKCU|HKCR|HKU|HKCC):|\b(Set-ItemProperty|New-ItemProperty|Remove-ItemProperty|Set-Item|New-Item|Remove-Item)\b.*Registry::)~"},
            {S::Command, V::Danger, "Windows", "creates or changes user accounts, passwords or administrator rights",
             R"~(\bnet1?(\.exe)?\s+(user|localgroup|group)\b.*\s/(add|del|delete|active)\b|\bnet1?(\.exe)?\s+user\s+[^/\s]\S*\s+[^/\s]|\b(New-LocalUser|Remove-LocalUser|Set-LocalUser|Enable-LocalUser|Disable-LocalUser|Rename-LocalUser|Add-LocalGroupMember|Remove-LocalGroupMember)\b|\bwmic\b.*\buseraccount\b.*\bset\b)~"},
            {S::Command, V::Danger, "Linux", "creates or changes user accounts, passwords or administrator rights",
             R"~(\b(useradd|userdel|usermod|adduser|deluser|groupadd|groupdel|groupmod|gpasswd|chpasswd|passwd|visudo)\b|/etc/sudoers)~"},
            {S::Command, V::Danger, "Any", "runs with administrator (root) rights",
             R"~(\bsudo\b|\bsu\s+(-|root\b)|\bpkexec\b|\bdoas\b|-Verb\s+['"]?RunAs\b|\brunas(\.exe)?\s+/)~"},
            {S::Command, V::Danger, "Any", "changes who can read or change files",
             R"~(\b(icacls|cacls|xcacls)\b.*/(grant|deny|remove|setowner|reset|inheritance)|\btakeown\b|\bSet-Acl\b|\bchmod\b.*\s(-R\b|[0-7]?777\b|[0-7]?666\b|[ugoa]*\+[rwx]*[ws])|\b(chown|chgrp)\b.*\s-R\b|\bchattr\b)~"},
            {S::Command, V::Danger, "Windows", "turns off antivirus, firewall or other protection",
             R"~(\bSet-MpPreference\b.*-Disable|\bAdd-MpPreference\b.*-Exclusion|\bnetsh\b.*\b(advfirewall|firewall)\b.*\b(off|disable|delete|add|reset)\b|\bSet-NetFirewallProfile\b.*-Enabled\s+['"]?(\$false|false|0)\b|\b(New|Set|Remove|Disable)-NetFirewallRule\b|\b(sc|Stop-Service|Set-Service)\b.*\b(WinDefend|mpssvc|wscsvc|Sense|WdNisSvc|SecurityHealthService)\b|\bSet-ExecutionPolicy\b|\b(DisableAntiSpyware|DisableRealtimeMonitoring|EnableLUA|ConsentPromptBehaviorAdmin)\b)~"},
            {S::Command, V::Danger, "Linux", "turns off antivirus, firewall or other protection",
             R"~(\bufw\s+(disable|reset)\b|\biptables\s+(-F|--flush|-X|-P\s+\w+\s+ACCEPT)\b|\bsetenforce\s+0\b|\bsystemctl\s+(stop|disable|mask)\s+(firewalld|ufw|apparmor|auditd|clamav\S*)\b|\baa-disable\b)~"},
            {S::Command, V::Danger, "Windows", "deletes backups, restore points or event logs",
             R"~(\bvssadmin\b.*\b(delete|resize)\b|\bwmic\b.*\bshadowcopy\b.*\bdelete\b|\bwbadmin\b.*\bdelete\b|\b(Disable-ComputerRestore|Remove-ComputerRestorePoint|Clear-EventLog|Remove-EventLog)\b|\bwevtutil(\.exe)?\s+(cl|clear-log)\b|\bWin32_ShadowCopy\b.*\b(Delete|Remove))~"},
            {S::Command, V::Danger, "Linux", "erases logs or the command history",
             R"~(\bhistory\s+-c\b|\bunset\s+HISTFILE\b|>\s*/var/log/|\b(rm|shred|truncate)\b.*/var/log/|\bjournalctl\b.*--vacuum)~"},
            {S::Command, V::Danger, "Windows", "changes how the computer starts (boot settings)",
             R"~(\b(bcdedit|bootsect|bcdboot|bootrec)\b|\breagentc\b.*/disable)~"},
            {S::Command, V::Danger, "Any", "makes a program start by itself (startup, scheduled task or service)",
             R"~(\bschtasks(\.exe)?\s+/(create|change)\b|\b(Register-ScheduledTask|Set-ScheduledTask|New-ScheduledTaskAction|New-Service|Register-WmiEvent)\b|\bsc(\.exe)?\s+create\b|\\+CurrentVersion\\+(Run|RunOnce|RunServices|Winlogon)\b|\\+Start Menu\\+Programs\\+Startup\b|\bshell:startup\b|__EventFilter|\bcrontab\b|/etc/cron|\bsystemctl\s+enable\b|/etc/systemd/system|/etc/init\.d/|/etc/rc\.local|\.config/autostart|>>?\s*\S*\.(bashrc|bash_profile|zshrc|profile)\b)~"},
            {S::Command, V::Danger, "Any", "downloads code from the internet and runs it",
             R"~(\b(curl|wget)\b.*\|\s*(sudo\s+)?(ba|z|da|k)?sh\b|\b(curl|wget)\b.*\|\s*(python\d?|perl|ruby|node)\b|\b(iwr|irm|Invoke-WebRequest|Invoke-RestMethod|DownloadString|DownloadFile|DownloadData|Start-BitsTransfer)\b.*\b(iex|Invoke-Expression|Start-Process|Invoke-Item)\b|\b(iex|Invoke-Expression)\b.*\b(iwr|irm|Invoke-WebRequest|Invoke-RestMethod|DownloadString|WebClient)\b|\bbitsadmin\b.*/transfer\b|\bcertutil\b.*-urlcache\b|\b(mshta|regsvr32|rundll32)\b.*https?:)~"},
            {S::Everywhere, V::Danger, "Python", "downloads code from the internet and runs it",
             R"~(\b(exec|eval)\s*\(.*\b(urlopen|requests\.get|urllib|httpx\.get)\b)~"},
            {S::Command, V::Danger, "Any", "lets another computer control this one (remote access)",
             R"~(\b(nc|ncat|netcat)\b.*\s-[a-z]*[ec]\b|/dev/tcp/|\bbash\s+-i\b|\bTCPClient\b|\b(Enable-PSRemoting|Enter-PSSession|New-PSSession)\b|\bwinrm\b.*\b(quickconfig|qc|set)\b|\bInvoke-Command\b.*-ComputerName\b|\bpsexec\b|\bfDenyTSConnections\b|\bnetsh\b.*\bportproxy\b|>>?\s*\S*authorized_keys\b|\bssh-copy-id\b)~"},
            {S::Everywhere, V::Danger, "Python", "lets another computer control this one (remote access)",
             R"~(\bos\.dup2\s*\(|\bpty\.spawn\s*\()~"},
            {S::Everywhere, V::Danger, "Any", "reads or changes stored passwords, keys or logins",
             R"~(\breg(\.exe)?\s+save\b.*\b(SAM|SECURITY|SYSTEM)\b|\b(mimikatz|sekurlsa|lsadump|procdump|comsvcs|lsass)\b|\bcmdkey\s+/list\b|\bvaultcmd\b|\bntds\.dit\b|\bnetsh\s+wlan\s+show\s+profile.*key\s*=\s*clear|[\\/]+(Login Data|Web Data|Cookies|Local State|key[34]\.db|logins\.json)\b|/etc/(shadow|gshadow)\b|[\\/]+\.ssh[\\/]+(id_\w+|authorized_keys)\b|[\\/]+\.aws[\\/]+credentials\b|\.git-credentials\b|[\\/]+\.netrc\b|[\\/]+\.kube[\\/]+config\b|\bkeyring\.get_password\b|\bwin32cred\b|\bCredRead\w*\b)~"},
            {S::Everywhere, V::Danger, "Any", "records what is typed (keylogger)",
             R"~(\b(GetAsyncKeyState|GetKeyState|SetWindowsHookEx\w*)\b|\bpynput\.keyboard\b|\bkeyboard\.(on_press|hook|record)\b)~"},
            {S::Everywhere, V::Danger, "Any", "crashes or freezes the computer",
             R"~(\b(taskkill|tskill|Stop-Process|spps|kill|wmic)\b.*\b(csrss|wininit|winlogon|smss|lsass|services|svchost)(\.exe)?\b|\bkill\s+-9\s+-?1\b|\bkillall5\b|/proc/sysrq-trigger|%0\s*\|\s*%0|:\s*\(\s*\)\s*\{\s*:\s*\|\s*:\s*&|\bwhile\s+(True|1)\s*:\s*os\.fork\s*\()~"},
            {S::Everywhere, V::Danger, "Any", "runs hidden (encoded) code that cannot be checked",
             R"~(\s-(e|ec|en|enc|enco|encod|encode|encoded|encodedc\w*)\s+['"]?[a-z0-9+/=]{20,}|\b(FromBase64String|b64decode|certutil\b.*-decode|base64\s+(-d|--decode))\b.*\b(iex|Invoke-Expression|exec|eval|Start-Process|Invoke|sh|bash|powershell|cmd|python\d?)\b|\b(exec|eval|iex|Invoke-Expression)\b.*\b(FromBase64String|b64decode)\b)~"},
            {S::Everywhere, V::Danger, "Linux", "changes system libraries or the kernel",
             R"~(\bLD_PRELOAD\b|/etc/ld\.so\.preload|\b(insmod|rmmod|modprobe)\b|\bsysctl\s+-w\b)~"},
            {S::Everywhere, V::Danger, "Windows", "changes Windows system files or installed programs",
             R"~(\\+Windows\\+|%SystemRoot%|%windir%|\$env:(windir|SystemRoot)|\\+(System32|SysWOW64)\\+|Program Files|ProgramData|\b[a-z]:\\+(Boot|Recovery)\b)~",
             kWriteVerbs},
            {S::Everywhere, V::Danger, "Linux", "changes Linux system files or installed programs",
             R"~((^|[\s'"=(:])/(etc|boot|usr|bin|sbin|lib|lib64|opt|root|var/lib|var/spool)(/|\b))~",
             kWriteVerbs},

            /* ---- CAUTION ---- */
            {S::Command, V::Caution, "Windows", "deletes files",
             R"~(\b(del|erase)\s+\S|\b(Remove-Item|ri)\b|\b(Clear-Content|clc|Clear-RecycleBin)\b)~"},
            {S::Command, V::Caution, "Linux", "deletes files",
             R"~(\b(rm|unlink|shred|srm)\s+\S|\bmv\b.*\s/dev/null\b|\btruncate\s+-s\s*0\b)~"},
            {S::Everywhere, V::Caution, "Python", "deletes files",
             R"~(\bos\.(remove|unlink|rmdir|removedirs)\s*\(|\.(unlink|rmdir)\s*\(|\bsend2trash\b)~"},
            {S::Command, V::Caution, "Any", "writes, moves or overwrites files",
             R"~(\b(Set-Content|Add-Content|Out-File|Copy-Item|Move-Item|Rename-Item|xcopy|robocopy|ren|rename|tee|mklink)\b|\b(copy|move|cp|mv)\s+\S|\bsed\s+-i\b|\bNew-Item\b.*-ItemType\s+['"]?(File|SymbolicLink|HardLink|Junction)\b)~"},
            {S::Everywhere, V::Caution, "Python", "writes, moves or overwrites files",
             R"~(\bopen\s*\(.*['"][wax]b?\+?['"]|\bopen\s*\(.*\bmode\s*=\s*['"][wax]|\.write_(text|bytes)\s*\(|\bshutil\.(copy\w*|move)\s*\(|\bos\.(rename|renames|replace)\s*\()~"},
            {S::Command, V::Caution, "Any", "shuts down, restarts or logs off the computer",
             R"~(\bshutdown(\.exe)?\s+([/-][srlhpfg]\b|now\b|\+\d)|\b(Stop-Computer|Restart-Computer)\b|\b(logoff|tsdiscon|reboot|poweroff|halt)\b|\binit\s+[06]\b|\bsystemctl\s+(poweroff|reboot|halt|suspend|hibernate)\b|\bLockWorkStation\b)~"},
            {S::Command, V::Caution, "Any", "stops programs or services",
             R"~(\b(taskkill|tskill|Stop-Process|spps|kill|pkill|killall|xkill)\b|\b(Stop-Service|Suspend-Service|Set-Service|Remove-Service|spsv)\b|\bsc(\.exe)?\s+(stop|delete|config|pause)\b|\bnet1?\s+stop\b|\bsystemctl\s+(stop|disable|mask|kill)\b|\bservice\s+\S+\s+stop\b|\bwmic\b.*\bprocess\b.*\b(delete|terminate)\b)~"},
            {S::Everywhere, V::Caution, "Python", "stops programs or services",
             R"~(\bos\.(kill|killpg)\s*\(|\bpsutil\b.*\.(kill|terminate)\s*\()~"},
            {S::Command, V::Caution, "Any", "installs or removes software",
             R"~(\b(winget|choco|scoop)\s+(install|uninstall|upgrade|remove)\b|\bmsiexec\b|\b(Install|Uninstall|Update)-(Package|Module|Script)\b|\b(Add|Remove)-AppxPackage\b|\b(Enable|Disable)-WindowsOptionalFeature\b|\b(Install|Uninstall)-WindowsFeature\b|\bdism\b.*/(enable|disable|remove|add)-|\bwusa\b|\bpip\d*(\.\d+)?\s+(install|uninstall|download)\b|-m\s+pip\s+(install|uninstall|download)\b|\bnpm\s+(install|i|uninstall|update)\b|\b(yarn|pnpm)\s+(add|install|global)\b|\bconda\s+(install|remove|update)\b|\b(gem|cargo|go)\s+install\b|\b(apt|apt-get|aptitude|yum|dnf|zypper|snap|flatpak)\s+(install|remove|purge|erase|upgrade|dist-upgrade|autoremove|reinstall)\b|\bpacman\s+-[SRU]|\bapk\s+(add|del)\b|\b(dpkg|rpm)\s+-(i|e|U)\b)~"},
            {S::Command, V::Caution, "Any", "uses the network: data could leave this computer, or files be downloaded",
             R"~(\b(Invoke-WebRequest|Invoke-RestMethod|iwr|irm|Start-BitsTransfer|Send-MailMessage)\b|\bNet\.(WebClient|Http\w*|Sockets|Mail)\b|\b(bitsadmin|certutil)\b.*\b(transfer|urlcache)\b|\b(curl|wget|nc|ncat|netcat|socat|scp|sftp|ssh|telnet|ftp|tftp|rsync)(\.exe)?\s|\b(https?|ftp)://|\\\\[a-z0-9.-]+\\)~"},
            {S::Everywhere, V::Caution, "Python", "uses the network: data could leave this computer, or files be downloaded",
             R"~(\bimport\s+.*\b(requests|urllib\d?|httpx|aiohttp|ftplib|smtplib|telnetlib|socket|paramiko|websockets?|pycurl|pysftp)\b|\bimport\s+http\.client\b|\bfrom\s+(requests|urllib\d?|httpx|aiohttp|http|ftplib|smtplib|telnetlib|socket|paramiko|websockets?)\b|\burl(open|retrieve)\s*\()~"},
            {S::Command, V::Caution, "Windows", "changes environment variables for good (PATH and others)",
             R"~(\bsetx\b|SetEnvironmentVariable\s*\(.*\b(Machine|User)\b|\[EnvironmentVariableTarget\]::(Machine|User))~"},
            {S::Command, V::Caution, "Any", "changes network settings",
             R"~(\bnetsh\b.*\b(interface|int|winhttp|wlan|ip|dns)\b.*\b(set|add|delete|reset|connect|disconnect)\b|\b(Set-DnsClientServerAddress|Set-NetIPAddress|New-NetIPAddress|Remove-NetIPAddress|Set-NetAdapter|Disable-NetAdapter|Enable-NetAdapter|Set-NetIPInterface|Set-NetConnectionProfile|New-NetRoute|Remove-NetRoute)\b|\bipconfig\s+/(release|renew|flushdns)\b|\broute\s+(add|delete|change|-f)\b|\b(ifconfig|ip\s+link)\b.*\b(down|up)\b|\bnmcli\b.*\b(down|delete|modify|off)\b)~"},
            {S::Command, V::Caution, "Windows", "changes the system through WMI",
             R"~(\b(Invoke-WmiMethod|Invoke-CimMethod|Set-WmiInstance|Set-CimInstance|Remove-WmiObject|Remove-CimInstance)\b|\bwmic\b.*\b(call|delete|set)\b)~"},
            {S::Everywhere, V::Caution, "Any", "runs text as code, so what it runs cannot be checked",
             R"~(\b(Invoke-Expression|iex)\b|\[ScriptBlock\]::Create\b|\bAdd-Type\b|\b(exec|eval)\s*\(|\bcompile\s*\(.*['"]exec['"]|\b__import__\s*\(|\bimportlib\.import_module\s*\(|\bctypes\b|\bsource\s+<\(|\beval\s+["'$])~"},
            {S::Everywhere, V::Caution, "Python", "changes who can read or change files",
             R"~(\bos\.(chmod|chown|lchown|chflags)\s*\()~"},
        };
        return rules;
    }

    // Compiled once, on first use; std::regex is safe to search from several threads.
    static const std::vector<CompiledRule>& CompiledRules() {
        static const std::vector<CompiledRule> compiled = [] {
            const auto flags = std::regex::ECMAScript | std::regex::icase | std::regex::optimize;
            std::vector<CompiledRule> out;
            for (const Rule& rule : RuleTable()) {
                CompiledRule c{&rule, std::regex(rule.pattern, flags), std::regex()};
                if (rule.also) {
                    c.also = std::regex(rule.also, flags);
                }
                out.push_back(std::move(c));
            }
            return out;
        }();
        return compiled;
    }

    static bool Matches(const CompiledRule& rule, const std::string& line) {
        for (size_t start = 0;; start += kChunkStep) {
            const char* begin = line.data() + start;
            const char* end   = begin + std::min(kChunkBytes, line.size() - start);
            if (std::regex_search(begin, end, rule.pattern) &&
                (!rule.def->also || std::regex_search(begin, end, rule.also))) {
                return true;
            }
            if (start + kChunkBytes >= line.size()) {
                return false;
            }
        }
    }

    /*
        One finding per harm: a later line with the same harm is counted in it, and DANGER replaces CAUTION.
        A line already shown for a DANGER finding gives no CAUTION one: `Remove-Item x -Recurse` is a recursive
        delete, and saying it also "deletes files" adds nothing. The DANGER rules come first in the table, so they
        are all known by then.
    */
    static void Add(Report& report, const Rule& rule, int line, const std::vector<std::string>& physical) {
        if (rule.severity == Severity::Caution) {
            for (const Finding& f : report.findings) {
                if (f.severity == Severity::Danger &&
                    (f.line == line || std::find(f.more_lines.begin(), f.more_lines.end(), line) != f.more_lines.end())) {
                    return;
                }
            }
        }
        for (Finding& f : report.findings) {
            if (std::strcmp(f.harm, rule.harm) != 0) {
                continue;
            }
            if (f.severity == Severity::Caution && rule.severity == Severity::Danger) {
                f = {rule.severity, rule.ecosystem, rule.harm, line, Evidence(physical, line), {}};
            } else if (line != f.line &&
                       std::find(f.more_lines.begin(), f.more_lines.end(), line) == f.more_lines.end()) {
                f.more_lines.push_back(line);
            }
            return;
        }
        report.findings.push_back({rule.severity, rule.ecosystem, rule.harm, line, Evidence(physical, line), {}});
    }

    static bool IsPython(const std::string& language) {
        std::string lang;
        for (unsigned char c : language) {
            lang += static_cast<char>(std::tolower(c));
        }
        return lang == "python" || lang == "py";
    }

    /*
        The Python code with everything but the text of its string literals blanked out, line breaks kept:
        `subprocess.run(["rm", "-rf", "/"])  # tidy` reads `                 rm    -rf    /             `.
        Escapes stay as written ("C:\\Windows" keeps both backslashes). Line numbers do not change.
    */
    static std::string PythonStrings(const std::string& code) {
        std::string out(code.size(), ' ');
        size_t i = 0;
        while (i < code.size()) {
            const char c = code[i];
            if (c == '\n') {
                out[i++] = '\n';
                continue;
            }
            if (c == '#') {
                while (i < code.size() && code[i] != '\n') {
                    ++i;
                }
                continue;
            }
            if (c != '\'' && c != '"') {
                ++i;
                continue;
            }
            const std::string triple(3, c);
            const bool is_triple = code.compare(i, 3, triple) == 0;
            i += is_triple ? 3 : 1;
            while (i < code.size()) {
                if (code[i] == '\\' && i + 1 < code.size()) {
                    out[i]     = code[i];
                    out[i + 1] = code[i + 1];
                    i += 2;
                    continue;
                }
                if (is_triple ? code.compare(i, 3, triple) == 0 : code[i] == c) {
                    i += is_triple ? 3 : 1;
                    break;
                }
                if (code[i] == '\n' && !is_triple) {
                    break;  // unterminated: the line ends it
                }
                out[i] = code[i];
                ++i;
            }
        }
        return out;
    }

    static std::vector<std::string> PhysicalLines(const std::string& text) {
        std::vector<std::string> lines;
        size_t start = 0;
        for (;;) {
            const size_t end = text.find('\n', start);
            lines.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) {
                return lines;
            }
            start = end + 1;
        }
    }

    /*
        The lines to search, a line that ends with a continuation character joined to the next one: `
        (PowerShell), ^ (cmd), \ (Python, shells). A command split over several lines is then read whole.
    */
    static std::vector<Line> LogicalLines(const std::string& text) {
        std::vector<Line> lines;
        bool joining = false;
        int  number  = 0;
        for (std::string line : PhysicalLines(text)) {
            ++number;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const bool continues = !line.empty() && (line.back() == '`' || line.back() == '^' || line.back() == '\\');
            if (continues) {
                line.pop_back();
            }
            if (joining && !lines.empty()) {
                lines.back().text += ' ' + line;
            } else {
                lines.push_back({number, line});
            }
            joining = continues;
        }
        return lines;
    }

    // The original line, trimmed and cut at a character boundary.
    static std::string Evidence(const std::vector<std::string>& physical, int line) {
        if (line < 1 || static_cast<size_t>(line) > physical.size()) {
            return {};
        }
        const std::string& s = physical[line - 1];
        const size_t first = s.find_first_not_of(" \t\r");
        if (first == std::string::npos) {
            return {};
        }
        std::string text = s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
        if (text.size() > kEvidenceBytes) {
            size_t end = kEvidenceBytes;
            while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
                --end;
            }
            text = text.substr(0, end) + " ...";
        }
        return text;
    }
};
