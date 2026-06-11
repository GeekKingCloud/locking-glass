using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;

namespace LockingGlass.WindowsInstallerBootstrapper
{
    internal static class Program
    {
        [STAThread]
        private static int Main(string[] args)
        {
            var quiet = InstallerOptions.ContainsQuietFlag(args);
            var mode = BootstrapperMode.Install;
            try
            {
                mode = GetBootstrapperMode();
                var options = InstallerOptions.Parse(args, mode);
                quiet = options.Quiet;
                var extractDirectory = options.ExtractDirectory ?? CreateExtractionDirectory();

                ExtractPayload(extractDirectory);
                int exitCode;
                var deleteExtractionDirectory = true;
                using (var payloadLocks = HoldExtractedPayloadReadLocks(extractDirectory))
                {
                    ValidateExtractedPayload(extractDirectory);

                    if (options.ExtractOnly)
                    {
                        Console.WriteLine(
                            "Extracted Locking Glass setup payload to: " + extractDirectory);
                        return 0;
                    }

                    switch (mode)
                    {
                        case BootstrapperMode.Uninstall:
                            exitCode = RunUninstaller(
                                extractDirectory,
                                options.InstallDirectory,
                                options.RemoveUserData,
                                options.Quiet);
                            break;
                        case BootstrapperMode.Run:
                            exitCode = RunApp(
                                extractDirectory,
                                options.AppArguments,
                                out deleteExtractionDirectory);
                            break;
                        default:
                            exitCode = RunInstaller(
                                extractDirectory,
                                options.InstallDirectory,
                                options.EnableAutostart,
                                options.LaunchAfterInstall,
                                options.Quiet);
                            break;
                    }
                }

                if (deleteExtractionDirectory)
                {
                    TryDeleteExtractionDirectory(extractDirectory);
                }
                return exitCode;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("Locking Glass bootstrapper failed: " + ex.Message);
                if (!quiet)
                {
                    ShowErrorMessage(GetFailureTitle(mode), ex.Message);
                }

                return 1;
            }
        }

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int MessageBoxW(
            IntPtr owner,
            string text,
            string caption,
            uint type);

        private const uint MessageBoxOk = 0x00000000;
        private const uint MessageBoxIconError = 0x00000010;
        private const uint MessageBoxIconInformation = 0x00000040;
        private const uint MessageBoxTaskModal = 0x00002000;

        private static BootstrapperMode GetBootstrapperMode()
        {
            foreach (var metadata in Assembly.GetExecutingAssembly()
                         .GetCustomAttributes<AssemblyMetadataAttribute>())
            {
                if (!string.Equals(
                        metadata.Key,
                        "LockingGlassBootstrapperMode",
                        StringComparison.Ordinal))
                {
                    continue;
                }

                if (string.Equals(metadata.Value, "uninstall", StringComparison.OrdinalIgnoreCase))
                {
                    return BootstrapperMode.Uninstall;
                }

                if (string.Equals(metadata.Value, "install", StringComparison.OrdinalIgnoreCase))
                {
                    return BootstrapperMode.Install;
                }

                if (string.Equals(metadata.Value, "run", StringComparison.OrdinalIgnoreCase))
                {
                    return BootstrapperMode.Run;
                }

                throw new InvalidOperationException(
                    "Unknown Locking Glass bootstrapper mode: " + metadata.Value);
            }

            return BootstrapperMode.Install;
        }

        private static string CreateExtractionDirectory()
        {
            var extractionRoot = Path.Combine(
                Path.GetTempPath(),
                "Locking Glass Setup",
                DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(extractionRoot);
            return extractionRoot;
        }

        private static void ExtractPayload(string extractionDirectory)
        {
            Directory.CreateDirectory(extractionDirectory);

            var assembly = Assembly.GetExecutingAssembly();
            using var payloadStream = assembly.GetManifestResourceStream("payload.zip")
                ?? throw new InvalidOperationException(
                    "The installer payload was not embedded in the setup executable.");

            var payloadPath = Path.Combine(extractionDirectory, "payload.zip");
            using (var payloadFile = File.Create(payloadPath))
            {
                payloadStream.CopyTo(payloadFile);
            }

            // Treat the embedded zip as untrusted even though we built it. The
            // bootstrapper is the last line of defense before writing files.
            ValidatePayloadZip(payloadPath);
            ZipFile.ExtractToDirectory(payloadPath, extractionDirectory);
            File.Delete(payloadPath);
        }

        private static void ValidatePayloadZip(string payloadPath)
        {
            using var archive = ZipFile.OpenRead(payloadPath);
            var seenEntries = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var entry in archive.Entries)
            {
                // The payload is expected to be a flat file set. Reject nested
                // or rooted entries before extraction so Zip Slip-style paths
                // cannot escape the temp extraction directory.
                if (string.IsNullOrWhiteSpace(entry.FullName) ||
                    entry.FullName.EndsWith("/", StringComparison.Ordinal) ||
                    entry.FullName.EndsWith("\\", StringComparison.Ordinal))
                {
                    throw new InvalidOperationException(
                        "The installer payload contains an unexpected directory entry.");
                }

                if (Path.IsPathRooted(entry.FullName))
                {
                    throw new InvalidOperationException(
                        "The installer payload contains a rooted entry: " + entry.FullName);
                }

                if (entry.FullName.IndexOfAny(new[] { '/', '\\' }) >= 0)
                {
                    throw new InvalidOperationException(
                        "The installer payload contains a nested entry: " + entry.FullName);
                }

                if (!IsValidPayloadFileName(entry.FullName))
                {
                    throw new InvalidOperationException(
                        "The installer payload contains an invalid entry name: " +
                        entry.FullName);
                }

                if (!seenEntries.Add(entry.FullName))
                {
                    throw new InvalidOperationException(
                        "The installer payload contains a duplicate entry: " + entry.FullName);
                }
            }
        }

        private static bool IsValidPayloadFileName(string fileName)
        {
            return !string.IsNullOrWhiteSpace(fileName) &&
                !Path.IsPathRooted(fileName) &&
                fileName.IndexOfAny(new[] { '/', '\\', ':' }) < 0 &&
                fileName != "..";
        }

        private static PayloadReadLocks HoldExtractedPayloadReadLocks(
            string extractionDirectory)
        {
            var lockedFiles = new List<FileStream>();
            try
            {
                foreach (var filePath in Directory.GetFiles(extractionDirectory))
                {
                    lockedFiles.Add(new FileStream(
                        filePath,
                        FileMode.Open,
                        FileAccess.Read,
                        FileShare.Read));
                }

                return new PayloadReadLocks(lockedFiles);
            }
            catch
            {
                foreach (var lockedFile in lockedFiles)
                {
                    lockedFile.Dispose();
                }

                throw;
            }
        }

        private static void ValidateExtractedPayload(string extractionDirectory)
        {
            var manifestPath =
                Path.Combine(extractionDirectory, "LOCKING_GLASS_PAYLOAD_MANIFEST.txt");
            if (!File.Exists(manifestPath))
            {
                throw new InvalidOperationException(
                    "The extracted payload is missing LOCKING_GLASS_PAYLOAD_MANIFEST.txt.");
            }

            var expected = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach (var line in File.ReadAllLines(manifestPath))
            {
                var separator = line.IndexOf("  ", StringComparison.Ordinal);
                if (separator <= 0 || separator + 2 >= line.Length)
                {
                    throw new InvalidOperationException(
                        "The payload manifest contains an invalid line: " + line);
                }

                var hash = line.Substring(0, separator);
                var fileName = line.Substring(separator + 2);
                if (!IsValidPayloadFileName(fileName) ||
                    fileName == "LOCKING_GLASS_PAYLOAD_MANIFEST.txt")
                {
                    throw new InvalidOperationException(
                        "The payload manifest contains an invalid file name: " + fileName);
                }

                expected[fileName] = hash;
            }

            // The manifest check catches accidental packaging drift and also
            // proves the extracted bytes are exactly the files we hashed during
            // staging.
            foreach (var filePath in Directory.GetFiles(extractionDirectory))
            {
                var fileName = Path.GetFileName(filePath);
                if (fileName == "LOCKING_GLASS_PAYLOAD_MANIFEST.txt")
                {
                    continue;
                }

                if (!expected.TryGetValue(fileName, out var expectedHash))
                {
                    throw new InvalidOperationException(
                        "The extracted payload contains an unexpected file: " + fileName);
                }

                var actualHash = ComputeSha256(filePath);
                if (!string.Equals(actualHash, expectedHash, StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidOperationException(
                        "The extracted payload hash did not match for " + fileName + ".");
                }

                expected.Remove(fileName);
            }

            if (expected.Count > 0)
            {
                throw new InvalidOperationException(
                    "The extracted payload is missing file(s) listed in the manifest.");
            }
        }

        private static string ComputeSha256(string filePath)
        {
            using var sha256 = SHA256.Create();
            using var stream = File.OpenRead(filePath);
            var hashBytes = sha256.ComputeHash(stream);
            return BitConverter.ToString(hashBytes).Replace("-", "").ToLowerInvariant();
        }

        private static int RunInstaller(
            string extractionDirectory,
            string? installDirectory,
            bool enableAutostart,
            bool launchAfterInstall,
            bool quiet)
        {
            var installerScript = Path.Combine(extractionDirectory, "Install-LockingGlass.ps1");
            if (!File.Exists(installerScript))
            {
                throw new InvalidOperationException(
                    "The extracted payload is missing Install-LockingGlass.ps1.");
            }

            var startInfo = new ProcessStartInfo
            {
                FileName = ResolveWindowsPowerShellPath(),
                WorkingDirectory = extractionDirectory,
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };

            startInfo.ArgumentList.Add("-NoProfile");
            startInfo.ArgumentList.Add("-ExecutionPolicy");
            startInfo.ArgumentList.Add("Bypass");
            startInfo.ArgumentList.Add("-File");
            startInfo.ArgumentList.Add(installerScript);
            startInfo.ArgumentList.Add("-SourceDir");
            startInfo.ArgumentList.Add(extractionDirectory);

            if (!string.IsNullOrWhiteSpace(installDirectory))
            {
                startInfo.ArgumentList.Add("-InstallDir");
                startInfo.ArgumentList.Add(installDirectory);
            }

            if (!enableAutostart)
            {
                startInfo.ArgumentList.Add("-NoAutostart");
            }

            if (launchAfterInstall)
            {
                startInfo.ArgumentList.Add("-LaunchAfterInstall");
            }

            RunCheckedChildProcess(
                startInfo,
                "Failed to launch the Locking Glass installer script.",
                "Locking Glass installation");

            Console.WriteLine("Locking Glass installation completed.");
            if (!quiet)
            {
                ShowInformationMessage(
                    "Locking Glass installed",
                    BuildInstallerSuccessMessage(enableAutostart, launchAfterInstall));
            }

            return 0;
        }

        private static int RunUninstaller(
            string extractionDirectory,
            string? installDirectory,
            bool removeUserData,
            bool quiet)
        {
            var uninstallerScript =
                Path.Combine(extractionDirectory, "Uninstall-LockingGlass.ps1");
            if (!File.Exists(uninstallerScript))
            {
                throw new InvalidOperationException(
                    "The extracted payload is missing Uninstall-LockingGlass.ps1.");
            }

            var startInfo = new ProcessStartInfo
            {
                FileName = ResolveWindowsPowerShellPath(),
                WorkingDirectory = extractionDirectory,
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };

            startInfo.ArgumentList.Add("-NoProfile");
            startInfo.ArgumentList.Add("-ExecutionPolicy");
            startInfo.ArgumentList.Add("Bypass");
            startInfo.ArgumentList.Add("-File");
            startInfo.ArgumentList.Add(uninstallerScript);

            if (!string.IsNullOrWhiteSpace(installDirectory))
            {
                startInfo.ArgumentList.Add("-InstallDir");
                startInfo.ArgumentList.Add(installDirectory);
            }

            if (removeUserData)
            {
                startInfo.ArgumentList.Add("-RemoveUserData");
            }

            if (quiet)
            {
                startInfo.ArgumentList.Add("-Quiet");
            }

            RunCheckedChildProcess(
                startInfo,
                "Failed to launch the Locking Glass uninstaller script.",
                "Locking Glass uninstallation");

            Console.WriteLine("Locking Glass uninstallation completed.");
            if (!quiet)
            {
                ShowInformationMessage(
                    "Locking Glass uninstalled",
                    "Locking Glass has been removed for the current user.");
            }

            return 0;
        }

        private static int RunApp(
            string extractionDirectory,
            IReadOnlyList<string> appArguments,
            out bool deleteExtractionDirectory)
        {
            deleteExtractionDirectory = true;
            var appPath = Path.Combine(extractionDirectory, "Locking Glass.exe");
            if (!File.Exists(appPath))
            {
                throw new InvalidOperationException(
                    "The extracted payload is missing Locking Glass.exe.");
            }

            var startInfo = new ProcessStartInfo
            {
                FileName = appPath,
                WorkingDirectory = extractionDirectory,
                UseShellExecute = false
            };

            if (appArguments.Count == 0)
            {
                startInfo.ArgumentList.Add("--background");
                startInfo.Environment["LOCKING_GLASS_STARTUP_NOTICE"] = "runner";
                using var backgroundProcess = Process.Start(startInfo)
                    ?? throw new InvalidOperationException(
                        "Failed to launch the Locking Glass app.");

                if (backgroundProcess.WaitForExit(5000))
                {
                    if (backgroundProcess.ExitCode != 0)
                    {
                        throw new InvalidOperationException(
                            "Locking Glass exited with code " +
                            backgroundProcess.ExitCode + ".");
                    }

                    return 0;
                }

                // The single-use app now owns this extracted payload. Keep the
                // directory so the live session can use its helper DLL, script,
                // and probe executable after this bootstrapper exits.
                deleteExtractionDirectory = false;
                return 0;
            }

            foreach (var argument in appArguments)
            {
                startInfo.ArgumentList.Add(argument);
            }

            using var process = Process.Start(startInfo)
                ?? throw new InvalidOperationException(
                    "Failed to launch the Locking Glass app.");

            process.WaitForExit();
            if (process.ExitCode != 0)
            {
                throw new InvalidOperationException(
                    "Locking Glass exited with code " + process.ExitCode + ".");
            }

            return 0;
        }

        private static void RunCheckedChildProcess(
            ProcessStartInfo startInfo,
            string launchFailureMessage,
            string operationName)
        {
            // The public setup executables are GUI-subsystem binaries. Hide the
            // internal console child while still preserving script output for
            // command-line smoke tests that redirect the bootstrapper streams.
            using var process = Process.Start(startInfo)
                ?? throw new InvalidOperationException(launchFailureMessage);

            var stdoutTask = process.StandardOutput.ReadToEndAsync();
            var stderrTask = process.StandardError.ReadToEndAsync();
            process.WaitForExit();

            var stdout = stdoutTask.GetAwaiter().GetResult();
            var stderr = stderrTask.GetAwaiter().GetResult();

            if (!string.IsNullOrWhiteSpace(stdout))
            {
                Console.Out.Write(stdout);
            }

            if (!string.IsNullOrWhiteSpace(stderr))
            {
                Console.Error.Write(stderr);
            }

            if (process.ExitCode != 0)
            {
                throw new InvalidOperationException(
                    operationName + " exited with code " + process.ExitCode + ".");
            }
        }

        private static string ResolveWindowsPowerShellPath()
        {
            var windowsDirectory = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
            if (!string.IsNullOrWhiteSpace(windowsDirectory))
            {
                var systemPowerShellPath = Path.Combine(
                    windowsDirectory,
                    "System32",
                    "WindowsPowerShell",
                    "v1.0",
                    "powershell.exe");
                if (File.Exists(systemPowerShellPath))
                {
                    return systemPowerShellPath;
                }
            }

            return @"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe";
        }

        private static string BuildInstallerSuccessMessage(
            bool enableAutostart,
            bool launchAfterInstall)
        {
            var paragraphs = new List<string>();
            if (launchAfterInstall)
            {
                paragraphs.Add(
                    "Locking Glass is now running." + Environment.NewLine +
                    "Use the icon in the notification area to lock/unlock monitors." +
                    Environment.NewLine +
                    "Note that it may be hidden by the arrow.");
            }
            else
            {
                paragraphs.Add(
                    "Locking Glass was installed. Start it from the Start Menu when " +
                    "you want to use the tray app.");
            }

            if (enableAutostart)
            {
                paragraphs.Add(
                    "Now installed, this app will also automatically run when you " +
                    "sign in, so it should always be available.");
            }
            else
            {
                paragraphs.Add("Startup was not changed for this install.");
            }

            return string.Join(Environment.NewLine + Environment.NewLine, paragraphs);
        }

        private static string GetFailureTitle(BootstrapperMode mode)
        {
            return mode switch
            {
                BootstrapperMode.Uninstall => "Locking Glass uninstall failed",
                BootstrapperMode.Run => "Locking Glass could not start",
                _ => "Locking Glass install failed"
            };
        }

        private static void ShowInformationMessage(string title, string message)
        {
            MessageBoxW(
                IntPtr.Zero,
                message,
                title,
                MessageBoxOk | MessageBoxIconInformation | MessageBoxTaskModal);
        }

        private static void ShowErrorMessage(string title, string message)
        {
            MessageBoxW(
                IntPtr.Zero,
                message,
                title,
                MessageBoxOk | MessageBoxIconError | MessageBoxTaskModal);
        }

        private static void TryDeleteExtractionDirectory(string extractionDirectory)
        {
            try
            {
                Directory.Delete(extractionDirectory, true);
            }
            catch
            {
            }
        }
    }

    internal sealed class InstallerOptions
    {
        public string? ExtractDirectory { get; private set; }

        public bool ExtractOnly { get; private set; }

        public string? InstallDirectory { get; private set; }

        public bool EnableAutostart { get; private set; } = true;

        public bool LaunchAfterInstall { get; private set; } = true;

        public bool RemoveUserData { get; private set; }

        public bool Quiet { get; private set; }

        public List<string> AppArguments { get; } = new List<string>();

        public static bool ContainsQuietFlag(string[] args)
        {
            foreach (var argument in args)
            {
                if (string.Equals(argument, "--quiet", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(argument, "--no-ui", StringComparison.OrdinalIgnoreCase))
                {
                    return true;
                }
            }

            return false;
        }

        public static InstallerOptions Parse(string[] args, BootstrapperMode mode)
        {
            var options = new InstallerOptions();

            for (var index = 0; index < args.Length; index += 1)
            {
                switch (args[index])
                {
                    case "--extract-only":
                        options.ExtractOnly = true;
                        options.ExtractDirectory = RequireValue(args, ref index);
                        break;
                    case "--install-dir":
                        options.InstallDirectory = RequireValue(args, ref index);
                        break;
                    case "--enable-autostart":
                        options.EnableAutostart = true;
                        break;
                    case "--no-autostart":
                        options.EnableAutostart = false;
                        break;
                    case "--no-launch-after-install":
                        options.LaunchAfterInstall = false;
                        break;
                    case "--remove-user-data":
                        options.RemoveUserData = true;
                        break;
                    case "--quiet":
                    case "--no-ui":
                        options.Quiet = true;
                        break;
                    default:
                        if (mode == BootstrapperMode.Run)
                        {
                            options.AppArguments.Add(args[index]);
                            break;
                        }

                        throw new InvalidOperationException("Unknown argument: " + args[index]);
                }
            }

            return options;
        }

        private static string RequireValue(string[] args, ref int index)
        {
            if (index + 1 >= args.Length)
            {
                throw new InvalidOperationException("Missing value after " + args[index] + ".");
            }

            index += 1;
            return args[index];
        }
    }

    internal enum BootstrapperMode
    {
        Install,
        Uninstall,
        Run
    }

    internal sealed class PayloadReadLocks : IDisposable
    {
        private readonly List<FileStream> _lockedFiles;

        public PayloadReadLocks(List<FileStream> lockedFiles)
        {
            _lockedFiles = lockedFiles;
        }

        public void Dispose()
        {
            foreach (var lockedFile in _lockedFiles)
            {
                lockedFile.Dispose();
            }
        }
    }
}
