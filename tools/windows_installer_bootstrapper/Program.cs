using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;

namespace LockingGlass.WindowsInstallerBootstrapper
{
    internal static class Program
    {
        [STAThread]
        private static int Main(string[] args)
        {
            try
            {
                var options = InstallerOptions.Parse(args);
                var extractDirectory = options.ExtractDirectory ?? CreateExtractionDirectory();

                ExtractPayload(extractDirectory);

                if (options.ExtractOnly)
                {
                    Console.WriteLine("Extracted LockingGlass setup payload to: " + extractDirectory);
                    return 0;
                }

                return RunInstaller(
                    extractDirectory,
                    options.InstallDirectory,
                    options.EnableAutostart,
                    options.LaunchAfterInstall);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("LockingGlass Setup failed: " + ex.Message);
                return 1;
            }
        }

        private static string CreateExtractionDirectory()
        {
            var extractionRoot = Path.Combine(
                Path.GetTempPath(),
                "LockingGlassSetup",
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

            ValidatePayloadZip(payloadPath);
            ZipFile.ExtractToDirectory(payloadPath, extractionDirectory);
            File.Delete(payloadPath);
            ValidateExtractedPayload(extractionDirectory);
        }

        private static void ValidatePayloadZip(string payloadPath)
        {
            using var archive = ZipFile.OpenRead(payloadPath);
            foreach (var entry in archive.Entries)
            {
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

                var segments = entry.FullName.Split(new[] { '/', '\\' });
                foreach (var segment in segments)
                {
                    if (segment == "..")
                    {
                        throw new InvalidOperationException(
                            "The installer payload contains a parent-directory entry: " +
                            entry.FullName);
                    }
                }
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
                if (Path.IsPathRooted(fileName) ||
                    fileName.IndexOfAny(new[] { '/', '\\' }) >= 0 ||
                    fileName == "LOCKING_GLASS_PAYLOAD_MANIFEST.txt")
                {
                    throw new InvalidOperationException(
                        "The payload manifest contains an invalid file name: " + fileName);
                }

                expected[fileName] = hash;
            }

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
            bool launchAfterInstall)
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
                UseShellExecute = false
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

            using var process = Process.Start(startInfo)
                ?? throw new InvalidOperationException(
                    "Failed to launch the LockingGlass installer script.");

            process.WaitForExit();
            if (process.ExitCode != 0)
            {
                throw new InvalidOperationException(
                    "LockingGlass installation exited with code " + process.ExitCode + ".");
            }

            TryDeleteExtractionDirectory(extractionDirectory);
            Console.WriteLine("LockingGlass installation completed.");
            return 0;
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

            return "powershell.exe";
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

        public static InstallerOptions Parse(string[] args)
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
                    default:
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
}
