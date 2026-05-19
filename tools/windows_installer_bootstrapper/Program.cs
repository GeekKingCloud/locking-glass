using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;

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

            ZipFile.ExtractToDirectory(payloadPath, extractionDirectory);
            File.Delete(payloadPath);
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
                FileName = "powershell.exe",
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

            if (enableAutostart)
            {
                startInfo.ArgumentList.Add("-EnableAutostart");
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

        public bool EnableAutostart { get; private set; }

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
