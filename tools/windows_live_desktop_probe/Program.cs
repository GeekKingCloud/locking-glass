using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Text;
using System.Threading;

[assembly: SupportedOSPlatform("windows")]

namespace LockingGlass.WindowsLiveDesktopProbe
{
    internal static class Program
    {
        [STAThread]
        private static int Main(string[] args)
        {
            try
            {
                var options = ProbeOptions.Parse(args);
                using (var logger = new ProbeLogger(options.LogPath))
                {
                    logger.Info(
                        options.WatchStream
                            ? "Locking Glass live desktop watch starting."
                            : "Locking Glass live desktop probe starting.");
                    logger.Info("Mode: " + (options.WatchStream ? "watch-stream" : "probe"));
                    logger.Info("Helper DLL: " + options.HelperDllPath);
                    logger.Info("Required desktop events: " + options.RequiredEvents);
                    logger.Info("Timeout seconds: " + options.TimeoutSeconds);
                    logger.Info("Auto-cycle enabled: " + options.AutoCycle);
                    logger.Info("Move exercise enabled: " + options.ExerciseMovePath);
                    logger.Info("Windows version: " + Environment.OSVersion.VersionString);

                    using (var controller = new VirtualDesktopAccessor(options.HelperDllPath))
                    {
                        if (options.WatchStream)
                        {
                            var runtime =
                                new ProbeRuntime(options, logger, controller, desktopManager: null);
                            return runtime.Run();
                        }

                        using (var desktopManager = new VirtualDesktopManagerClient())
                        {
                            var runtime =
                                new ProbeRuntime(options, logger, controller, desktopManager);
                            return runtime.Run();
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(ex.Message);
                return 1;
            }
        }
    }

    internal sealed class ProbeOptions
    {
        public ProbeOptions(
            string helperDllPath,
            string logPath,
            int requiredEvents,
            int timeoutSeconds,
            bool autoCycle,
            bool exerciseMovePath,
            bool watchStream)
        {
            HelperDllPath = helperDllPath;
            LogPath = logPath;
            RequiredEvents = requiredEvents;
            TimeoutSeconds = timeoutSeconds;
            AutoCycle = autoCycle;
            ExerciseMovePath = exerciseMovePath;
            WatchStream = watchStream;
        }

        public string HelperDllPath { get; }

        public string LogPath { get; }

        public int RequiredEvents { get; }

        public int TimeoutSeconds { get; }

        public bool AutoCycle { get; }

        public bool ExerciseMovePath { get; }

        public bool WatchStream { get; }

        public static ProbeOptions Parse(string[] args)
        {
            string? helperDllPath = null;
            string? logPath = null;
            var requiredEvents = 2;
            var timeoutSeconds = 20;
            var autoCycle = false;
            var exerciseMovePath = false;
            var watchStream = false;

            for (var index = 0; index < args.Length; index += 1)
            {
                switch (args[index])
                {
                    case "--helper-dll":
                        helperDllPath = RequireValue(args, ref index);
                        break;
                    case "--log":
                        logPath = RequireValue(args, ref index);
                        break;
                    case "--required-events":
                        requiredEvents =
                            ParseNonNegativeInt("--required-events", RequireValue(args, ref index));
                        break;
                    case "--timeout-seconds":
                        timeoutSeconds =
                            ParseNonNegativeInt("--timeout-seconds", RequireValue(args, ref index));
                        break;
                    case "--auto-cycle":
                        autoCycle = true;
                        break;
                    case "--exercise-move":
                        exerciseMovePath = true;
                        break;
                    case "--watch-stream":
                        watchStream = true;
                        break;
                    default:
                        throw new InvalidOperationException("Unknown argument: " + args[index]);
                }
            }

            if (string.IsNullOrWhiteSpace(helperDllPath))
            {
                throw new InvalidOperationException("Missing required argument --helper-dll.");
            }

            if (string.IsNullOrWhiteSpace(logPath))
            {
                throw new InvalidOperationException("Missing required argument --log.");
            }

            return new ProbeOptions(
                helperDllPath,
                logPath,
                requiredEvents,
                timeoutSeconds,
                autoCycle,
                exerciseMovePath,
                watchStream);
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

        private static int ParseNonNegativeInt(string argument, string value)
        {
            int parsed;
            if (!int.TryParse(value, out parsed) || parsed < 0)
            {
                throw new InvalidOperationException(
                    argument + " expects a non-negative integer, received '" + value + "'.");
            }

            return parsed;
        }
    }

    internal sealed class ProbeLogger : IDisposable
    {
        private readonly StreamWriter _writer;

        public ProbeLogger(string logPath)
        {
            var directory = Path.GetDirectoryName(logPath);
            if (!string.IsNullOrEmpty(directory))
            {
                Directory.CreateDirectory(directory);
            }

            _writer = new StreamWriter(logPath, false, Encoding.UTF8)
            {
                AutoFlush = true
            };
        }

        public void Info(string message)
        {
            var line = DateTimeOffset.Now.ToString("O") + " " + message;
            Console.WriteLine(line);
            _writer.WriteLine(line);
        }

        public void Dispose()
        {
            _writer.Dispose();
        }
    }

    internal sealed class VirtualDesktopAccessor : IDisposable
    {
        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate int GetDesktopCountDelegate();

        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate int GetCurrentDesktopNumberDelegate();

        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate int GetWindowDesktopNumberDelegate(IntPtr hwnd);

        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate int MoveWindowToDesktopNumberDelegate(IntPtr hwnd, int desktopNumber);

        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate int GoToDesktopNumberDelegate(int desktopNumber);

        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate int RegisterPostMessageHookDelegate(IntPtr listenerHwnd, int messageId);

        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate int UnregisterPostMessageHookDelegate(IntPtr listenerHwnd);

        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate int GetDesktopNameDelegate(
            int desktopNumber,
            IntPtr buffer,
            UIntPtr bufferLength);

        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate Guid GetDesktopIdByNumberDelegate(int desktopNumber);

        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate int CreateDesktopDelegate();

        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate int SetDesktopNameDelegate(int desktopNumber, string desktopName);

        [UnmanagedFunctionPointer(CallingConvention.Winapi)]
        private delegate int RemoveDesktopDelegate(int desktopNumber, int fallbackDesktopNumber);

        private readonly IntPtr _libraryHandle;
        private readonly GetDesktopCountDelegate _getDesktopCount;
        private readonly GetCurrentDesktopNumberDelegate _getCurrentDesktopNumber;
        private readonly GetWindowDesktopNumberDelegate _getWindowDesktopNumber;
        private readonly MoveWindowToDesktopNumberDelegate _moveWindowToDesktopNumber;
        private readonly GoToDesktopNumberDelegate _goToDesktopNumber;
        private readonly RegisterPostMessageHookDelegate _registerPostMessageHook;
        private readonly UnregisterPostMessageHookDelegate _unregisterPostMessageHook;
        private readonly GetDesktopNameDelegate _getDesktopName;
        private readonly GetDesktopIdByNumberDelegate _getDesktopIdByNumber;
        private readonly CreateDesktopDelegate _createDesktop;
        private readonly SetDesktopNameDelegate _setDesktopName;
        private readonly RemoveDesktopDelegate _removeDesktop;

        public VirtualDesktopAccessor(string helperDllPath)
        {
            if (!File.Exists(helperDllPath))
            {
                throw new InvalidOperationException(
                    "Locking Glass fails closed because the live helper DLL is missing: " +
                    helperDllPath);
            }

            _libraryHandle = NativeLibrary.Load(helperDllPath);
            _getDesktopCount = LoadRequiredDelegate<GetDesktopCountDelegate>("GetDesktopCount");
            _getCurrentDesktopNumber =
                LoadRequiredDelegate<GetCurrentDesktopNumberDelegate>("GetCurrentDesktopNumber");
            _getWindowDesktopNumber =
                LoadRequiredDelegate<GetWindowDesktopNumberDelegate>("GetWindowDesktopNumber");
            _moveWindowToDesktopNumber =
                LoadRequiredDelegate<MoveWindowToDesktopNumberDelegate>(
                    "MoveWindowToDesktopNumber");
            _goToDesktopNumber =
                LoadRequiredDelegate<GoToDesktopNumberDelegate>("GoToDesktopNumber");
            _registerPostMessageHook =
                LoadRequiredDelegate<RegisterPostMessageHookDelegate>(
                    "RegisterPostMessageHook");
            _unregisterPostMessageHook =
                LoadRequiredDelegate<UnregisterPostMessageHookDelegate>(
                    "UnregisterPostMessageHook");
            _getDesktopName = LoadRequiredDelegate<GetDesktopNameDelegate>("GetDesktopName");
            _getDesktopIdByNumber =
                LoadRequiredDelegate<GetDesktopIdByNumberDelegate>("GetDesktopIdByNumber");
            _createDesktop = LoadRequiredDelegate<CreateDesktopDelegate>("CreateDesktop");
            _setDesktopName = LoadRequiredDelegate<SetDesktopNameDelegate>("SetDesktopName");
            _removeDesktop = LoadRequiredDelegate<RemoveDesktopDelegate>("RemoveDesktop");
        }

        public int GetDesktopCount()
        {
            return _getDesktopCount();
        }

        public int GetCurrentDesktopNumber()
        {
            return _getCurrentDesktopNumber();
        }

        public int GetWindowDesktopNumber(IntPtr hwnd)
        {
            return _getWindowDesktopNumber(hwnd);
        }

        public int MoveWindowToDesktopNumber(IntPtr hwnd, int desktopNumber)
        {
            return _moveWindowToDesktopNumber(hwnd, desktopNumber);
        }

        public int GoToDesktopNumber(int desktopNumber)
        {
            return _goToDesktopNumber(desktopNumber);
        }

        public int RegisterPostMessageHook(IntPtr listenerHwnd, int messageId)
        {
            return _registerPostMessageHook(listenerHwnd, messageId);
        }

        public int UnregisterPostMessageHook(IntPtr listenerHwnd)
        {
            return _unregisterPostMessageHook(listenerHwnd);
        }

        public string GetDesktopName(int desktopNumber)
        {
            var buffer = new byte[1024];
            var pinned = GCHandle.Alloc(buffer, GCHandleType.Pinned);
            try
            {
                var result = _getDesktopName(
                    desktopNumber,
                    pinned.AddrOfPinnedObject(),
                    (UIntPtr)buffer.Length);
                if (result < 0)
                {
                    return string.Empty;
                }

                var terminator = Array.IndexOf(buffer, (byte)0);
                var length = terminator >= 0 ? terminator : buffer.Length;
                return Encoding.UTF8.GetString(buffer, 0, length).Trim();
            }
            finally
            {
                pinned.Free();
            }
        }

        public Guid GetDesktopIdByNumber(int desktopNumber)
        {
            return _getDesktopIdByNumber(desktopNumber);
        }

        public int CreateDesktop()
        {
            return _createDesktop();
        }

        public int SetDesktopName(int desktopNumber, string desktopName)
        {
            return _setDesktopName(desktopNumber, desktopName);
        }

        public int RemoveDesktop(int desktopNumber, int fallbackDesktopNumber)
        {
            return _removeDesktop(desktopNumber, fallbackDesktopNumber);
        }

        public void Dispose()
        {
            if (_libraryHandle != IntPtr.Zero)
            {
                NativeLibrary.Free(_libraryHandle);
            }
        }

        private T LoadRequiredDelegate<T>(string exportName) where T : class
        {
            IntPtr exportHandle;
            if (!NativeLibrary.TryGetExport(_libraryHandle, exportName, out exportHandle))
            {
                throw new InvalidOperationException(
                    "Locking Glass fails closed because VirtualDesktopAccessor.dll is missing export '" +
                    exportName + "'.");
            }

            var delegateObject =
                Marshal.GetDelegateForFunctionPointer(exportHandle, typeof(T)) as T;
            if (delegateObject == null)
            {
                throw new InvalidOperationException(
                    "Failed to bind delegate for export '" + exportName + "'.");
            }

            return delegateObject;
        }

        private T? LoadOptionalDelegate<T>(string exportName) where T : class
        {
            IntPtr exportHandle;
            if (!NativeLibrary.TryGetExport(_libraryHandle, exportName, out exportHandle))
            {
                return null;
            }

            return Marshal.GetDelegateForFunctionPointer(exportHandle, typeof(T)) as T;
        }
    }

    [ComImport]
    [Guid("A5CD92FF-29BE-454C-8D04-D82879FB3F1B")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IVirtualDesktopManager
    {
        int IsWindowOnCurrentVirtualDesktop(IntPtr topLevelWindow, out int onCurrentDesktop);

        int GetWindowDesktopId(IntPtr topLevelWindow, out Guid desktopId);

        int MoveWindowToDesktop(IntPtr topLevelWindow, ref Guid desktopId);
    }

    internal sealed class VirtualDesktopManagerClient : IDisposable
    {
        private static readonly Guid DesktopManagerClassId =
            new Guid("AA509086-5CA9-4C25-8F95-589D3C07B48A");

        private readonly IVirtualDesktopManager _desktopManager;

        public VirtualDesktopManagerClient()
        {
            var desktopManagerType = Type.GetTypeFromCLSID(DesktopManagerClassId, true);
            if (desktopManagerType == null)
            {
                throw new InvalidOperationException(
                    "Locking Glass fails closed because the VirtualDesktopManager class type " +
                    "could not be resolved.");
            }

            var desktopManager =
                Activator.CreateInstance(desktopManagerType) as IVirtualDesktopManager;
            if (desktopManager == null)
            {
                throw new InvalidOperationException(
                    "Locking Glass fails closed because IVirtualDesktopManager could not be created.");
            }

            _desktopManager = desktopManager;
        }

        public Guid GetWindowDesktopId(IntPtr windowHandle)
        {
            Guid desktopId;
            var hr = _desktopManager.GetWindowDesktopId(windowHandle, out desktopId);
            if (hr < 0)
            {
                Marshal.ThrowExceptionForHR(hr);
            }

            return desktopId;
        }

        public void MoveWindowToDesktop(IntPtr windowHandle, Guid desktopId)
        {
            var targetId = desktopId;
            var hr = _desktopManager.MoveWindowToDesktop(windowHandle, ref targetId);
            if (hr < 0)
            {
                Marshal.ThrowExceptionForHR(hr);
            }
        }

        public void Dispose()
        {
            if (_desktopManager != null && Marshal.IsComObject(_desktopManager))
            {
                Marshal.FinalReleaseComObject(_desktopManager);
            }
        }
    }

    internal sealed class ProbeRuntime
    {
        private const int DesktopChangedMessage = 0x1400 + 30;
        private const uint WM_TIMER = 0x0113;
        private const uint WM_DESTROY = 0x0002;
        private const uint WS_OVERLAPPED = 0x00000000;
        private const uint WS_CAPTION = 0x00C00000;
        private const uint WS_SYSMENU = 0x00080000;
        private const uint WS_MINIMIZEBOX = 0x00020000;
        private const uint WS_VISIBLE = 0x10000000;
        private const uint WS_EX_TOOLWINDOW = 0x00000080;
        private const int SW_SHOW = 5;
        private const uint TimeoutTimerId = 1;
        private const uint SwitchTimerId = 2;
        private const int MoveVerificationMaxPolls = 60;
        private const int MoveVerificationSleepMilliseconds = 50;

        private readonly ProbeOptions _options;
        private readonly ProbeLogger _logger;
        private readonly VirtualDesktopAccessor _controller;
        private readonly VirtualDesktopManagerClient? _desktopManager;
        private readonly WndProc _windowProc;
        private readonly string _className;
        private IntPtr _windowHandle;
        private bool _hookRegistered;
        private bool _switchBackScheduled;
        private bool _finished;
        private int _observedEvents;
        private int _initialDesktopNumber;
        private int _alternateDesktopNumber;
        private int _scheduledDesktopNumber;
        private Process? _moveTargetProcess;
        private string? _moveTargetFilePath;

        public ProbeRuntime(
            ProbeOptions options,
            ProbeLogger logger,
            VirtualDesktopAccessor controller,
            VirtualDesktopManagerClient? desktopManager)
        {
            _options = options;
            _logger = logger;
            _controller = controller;
            _desktopManager = desktopManager;
            _windowProc = HandleWindowMessage;
            _className = "LockingGlassLiveDesktopProbeWindow";
        }

        public int ExitCode { get; private set; } = 1;

        public int Run()
        {
            var instance = GetModuleHandle(null);
            if (instance == IntPtr.Zero)
            {
                throw new InvalidOperationException("GetModuleHandle failed.");
            }

            var windowClass = new WNDCLASSEX
            {
                cbSize = (uint)Marshal.SizeOf(typeof(WNDCLASSEX)),
                lpfnWndProc = _windowProc,
                hInstance = instance,
                lpszClassName = _className,
            };

            var classAtom = RegisterClassEx(ref windowClass);
            if (classAtom == 0)
            {
                throw new InvalidOperationException(
                    "RegisterClassEx failed with Win32 error " +
                    Marshal.GetLastWin32Error() + ".");
            }

            _windowHandle = CreateWindowEx(
                WS_EX_TOOLWINDOW,
                _className,
                _options.WatchStream
                    ? "Locking Glass Live Desktop Watch"
                    : "Locking Glass Live Desktop Probe",
                _options.WatchStream
                    ? WS_OVERLAPPED
                    : WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
                32,
                32,
                _options.WatchStream ? 1 : 360,
                _options.WatchStream ? 1 : 140,
                IntPtr.Zero,
                IntPtr.Zero,
                instance,
                IntPtr.Zero);
            if (_windowHandle == IntPtr.Zero)
            {
                throw new InvalidOperationException(
                    "CreateWindowEx failed with Win32 error " +
                    Marshal.GetLastWin32Error() + ".");
            }

            if (!_options.WatchStream)
            {
                ShowWindow(_windowHandle, SW_SHOW);
                UpdateWindow(_windowHandle);
                _logger.Info("Waiting 1000 ms for the probe window to settle on the shell.");
                Thread.Sleep(1000);
            }
            else
            {
                _logger.Info("Using a hidden message-only watch window for live desktop events.");
            }
            StartProbe();

            MSG message;
            while (true)
            {
                var result = GetMessage(out message, IntPtr.Zero, 0, 0);
                if (result == 0)
                {
                    break;
                }

                if (result == -1)
                {
                    throw new InvalidOperationException(
                        "GetMessage failed with Win32 error " +
                        Marshal.GetLastWin32Error() + ".");
                }

                TranslateMessage(ref message);
                DispatchMessage(ref message);
            }

            return ExitCode;
        }

        private IntPtr HandleWindowMessage(
            IntPtr hwnd,
            uint message,
            IntPtr wParam,
            IntPtr lParam)
        {
            if (message == (uint)DesktopChangedMessage)
            {
                HandleDesktopChanged(wParam.ToInt32(), lParam.ToInt32());
                return IntPtr.Zero;
            }

            if (message == WM_TIMER)
            {
                HandleTimer((uint)wParam.ToInt64());
                return IntPtr.Zero;
            }

            if (message == WM_DESTROY)
            {
                CleanupHook();
                PostQuitMessage(ExitCode);
                return IntPtr.Zero;
            }

            return DefWindowProc(hwnd, message, wParam, lParam);
        }

        private void HandleDesktopChanged(int oldDesktop, int newDesktop)
        {
            if (_finished)
            {
                return;
            }

            var oldDesktopName = oldDesktop >= 0 ? _controller.GetDesktopName(oldDesktop) : string.Empty;
            var newDesktopName = newDesktop >= 0 ? _controller.GetDesktopName(newDesktop) : string.Empty;
            var suffix = string.IsNullOrEmpty(newDesktopName)
                ? string.Empty
                : " name='" + newDesktopName + "'";
            var oldDesktopId = oldDesktop >= 0 ? _controller.GetDesktopIdByNumber(oldDesktop) : Guid.Empty;
            var newDesktopId = newDesktop >= 0 ? _controller.GetDesktopIdByNumber(newDesktop) : Guid.Empty;

            _observedEvents += 1;
            _logger.Info(
                "Live desktop event " + _observedEvents +
                ": raw " + oldDesktop + " -> " + newDesktop +
                "; user-facing " + (oldDesktop + 1) + " -> " + (newDesktop + 1) +
                "." + suffix);
            if (_options.WatchStream)
            {
                EmitWatchEvent(
                    oldDesktop,
                    oldDesktopId,
                    oldDesktopName,
                    newDesktop,
                    newDesktopId,
                    newDesktopName);
            }

            if (_options.AutoCycle &&
                !_switchBackScheduled &&
                newDesktop == _alternateDesktopNumber)
            {
                _switchBackScheduled = true;
                ScheduleDesktopSwitch(_initialDesktopNumber, 1200);
            }

            if (_options.RequiredEvents > 0 && _observedEvents >= _options.RequiredEvents)
            {
                Succeed("Captured the required number of live desktop-switch notifications.");
            }
        }

        private void HandleTimer(uint timerId)
        {
            KillTimer(_windowHandle, timerId);

            if (_finished)
            {
                return;
            }

            if (timerId == TimeoutTimerId)
            {
                FailClosed(
                    "Timed out after " + _options.TimeoutSeconds +
                    " seconds without collecting " + _options.RequiredEvents +
                    " live desktop events.");
                return;
            }

            if (timerId == SwitchTimerId)
            {
                IssueDesktopSwitch(_scheduledDesktopNumber);
            }
        }

        private void StartProbe()
        {
            _logger.Info(
                "Probe window handle: 0x" + _windowHandle.ToInt64().ToString("X"));

            var desktopCount = _controller.GetDesktopCount();
            if (desktopCount < 2)
            {
                FailClosed(
                    "Desktop count is below 2. Locking Glass cannot prove the live hook or " +
                    "move path on a single-desktop shell.");
                return;
            }

            _initialDesktopNumber = _controller.GetCurrentDesktopNumber();
            if (_initialDesktopNumber < 0)
            {
                FailClosed("The helper could not resolve the current virtual desktop number.");
                return;
            }

            _alternateDesktopNumber = _initialDesktopNumber == 0 ? 1 : 0;
            _logger.Info("Desktop count: " + desktopCount);
            _logger.Info("Current desktop: " + _initialDesktopNumber);
            _logger.Info(
                "Alternate desktop selected for proof: " + _alternateDesktopNumber);

            var registerResult =
                _controller.RegisterPostMessageHook(_windowHandle, DesktopChangedMessage);
            if (registerResult < 0)
            {
                FailClosed(
                    "RegisterPostMessageHook failed. Locking Glass must fail closed because the " +
                    "live notification source is unavailable.");
                return;
            }

            _hookRegistered = true;
            _logger.Info(
                "Registered VirtualDesktopAccessor post-message hook on message 0x" +
                DesktopChangedMessage.ToString("X") + ".");
            if (_options.WatchStream)
            {
                EmitWatchReady(
                    desktopCount,
                    _initialDesktopNumber,
                    _controller.GetDesktopIdByNumber(_initialDesktopNumber),
                    _controller.GetDesktopName(_initialDesktopNumber));
            }

            if (!_options.WatchStream && _options.ExerciseMovePath && !ExerciseMovePath())
            {
                return;
            }

            if (!_options.WatchStream && !ExerciseStagingDesktopLifecycle())
            {
                return;
            }

            if (_options.TimeoutSeconds > 0 &&
                SetTimer(
                    _windowHandle,
                    TimeoutTimerId,
                    (uint)(_options.TimeoutSeconds * 1000),
                    IntPtr.Zero) == UIntPtr.Zero)
            {
                FailClosed(
                    "SetTimer failed for the overall timeout, so the probe cannot guarantee " +
                    "fail-closed behavior.");
                return;
            }

            if (_options.AutoCycle)
            {
                ScheduleDesktopSwitch(_alternateDesktopNumber, 1200);
            }
            else
            {
                _logger.Info(
                    "Auto-cycle disabled. Switch desktops manually to generate live notifications.");
            }
        }

        private bool ExerciseMovePath()
        {
            if (_desktopManager == null)
            {
                FailClosed(
                    "The move-path exercise requires IVirtualDesktopManager, but the desktop manager client was unavailable.");
                return false;
            }

            IntPtr moveTargetHandle;
            string moveTargetLabel;
            if (!TryAcquireMoveTarget(out moveTargetHandle, out moveTargetLabel))
            {
                return false;
            }

            _logger.Info(
                "Move-path target HWND: 0x" + moveTargetHandle.ToInt64().ToString("X") +
                " (" + moveTargetLabel + ").");

            Guid initialDesktopId;
            try
            {
                initialDesktopId = _desktopManager.GetWindowDesktopId(moveTargetHandle);
            }
            catch (Exception ex)
            {
                FailClosed(
                    "IVirtualDesktopManager.GetWindowDesktopId failed before the move-path " +
                    "check: " + ex.Message);
                return false;
            }

            Guid alternateDesktopId;
            try
            {
                alternateDesktopId = _controller.GetDesktopIdByNumber(_alternateDesktopNumber);
            }
            catch (Exception ex)
            {
                FailClosed(
                    "VirtualDesktopAccessor.GetDesktopIdByNumber failed before the move-path " +
                    "check: " + ex.Message);
                return false;
            }

            _logger.Info(
                "Move-path exercise starting from desktop GUID " + initialDesktopId +
                " for HWND 0x" + moveTargetHandle.ToInt64().ToString("X") + ".");

            var moveAwayResult =
                _controller.MoveWindowToDesktopNumber(
                    moveTargetHandle,
                    _alternateDesktopNumber);
            if (moveAwayResult < 0)
            {
                FailClosed(
                    "VirtualDesktopAccessor.MoveWindowToDesktopNumber failed when moving the " +
                    "real target window to the alternate desktop.");
                return false;
            }

            Guid movedDesktopId;
            int movedDesktopNumber;
            if (!TryWaitForDesktopAssignment(
                    moveTargetHandle,
                    alternateDesktopId,
                    _alternateDesktopNumber,
                    "alternate move",
                    out movedDesktopId,
                    out movedDesktopNumber))
            {
                FailClosed(
                    "MoveWindowToDesktop succeeded but the probe window landed on desktop GUID " +
                    movedDesktopId + " with helper desktop " + movedDesktopNumber +
                    " after polling for the alternate move.");
                return false;
            }
            _logger.Info(
                "Move-path check after alternate move: COM reports desktop GUID " +
                movedDesktopId + " with helper desktop " + movedDesktopNumber + ".");

            var moveBackResult =
                _controller.MoveWindowToDesktopNumber(
                    moveTargetHandle,
                    _initialDesktopNumber);
            if (moveBackResult < 0)
            {
                FailClosed(
                    "VirtualDesktopAccessor.MoveWindowToDesktopNumber failed when returning the " +
                    "real target window to the starting desktop.");
                return false;
            }

            Guid returnedDesktopId;
            int returnedDesktopNumber;
            if (!TryWaitForDesktopAssignment(
                    moveTargetHandle,
                    initialDesktopId,
                    _initialDesktopNumber,
                    "return move",
                    out returnedDesktopId,
                    out returnedDesktopNumber))
            {
                FailClosed(
                    "MoveWindowToDesktop succeeded but the probe window returned to desktop GUID " +
                    returnedDesktopId + " with helper desktop " + returnedDesktopNumber +
                    " after polling for the return move.");
                return false;
            }
            _logger.Info(
                "Move-path check after return move: COM reports desktop GUID " +
                returnedDesktopId + " with helper desktop " + returnedDesktopNumber + ".");

            _logger.Info(
                "Move-path exercise succeeded with IVirtualDesktopManager plus the live helper's desktop lookup.");
            return true;
        }

        private bool ExerciseStagingDesktopLifecycle()
        {
            // The proof creates, names, resolves, and removes a disposable
            // desktop without moving user windows into it.
            var beforeCount = _controller.GetDesktopCount();
            var createdDesktopNumber = _controller.CreateDesktop();
            if (createdDesktopNumber < 0)
            {
                FailClosed(
                    "VirtualDesktopAccessor.CreateDesktop failed during the staging lifecycle exercise.");
                return false;
            }

            // Removing a desktop needs somewhere to send any accidental
            // occupants; prefer the desktop where the proof started.
            var removeFallbackDesktop = _initialDesktopNumber >= 0 ? _initialDesktopNumber : 0;
            var shouldRemoveCreatedDesktop = true;
            try
            {
                if (_controller.SetDesktopName(createdDesktopNumber, "Locking Glass Proof") < 0)
                {
                    FailClosed(
                        "VirtualDesktopAccessor.SetDesktopName failed during the staging lifecycle exercise.");
                    return false;
                }

                var resolvedName = _controller.GetDesktopName(createdDesktopNumber);
                if (resolvedName != "Locking Glass Proof")
                {
                    FailClosed(
                        "VirtualDesktopAccessor.GetDesktopName returned '" + resolvedName +
                        "' for the staging lifecycle exercise.");
                    return false;
                }

                var createdDesktopId = _controller.GetDesktopIdByNumber(createdDesktopNumber);
                if (createdDesktopId == Guid.Empty)
                {
                    FailClosed(
                        "VirtualDesktopAccessor.GetDesktopIdByNumber returned an empty GUID for the staging lifecycle exercise.");
                    return false;
                }

                if (_controller.RemoveDesktop(createdDesktopNumber, removeFallbackDesktop) < 0)
                {
                    FailClosed(
                        "VirtualDesktopAccessor.RemoveDesktop failed during the staging lifecycle exercise.");
                    return false;
                }
                shouldRemoveCreatedDesktop = false;

                var afterCount = _controller.GetDesktopCount();
                if (afterCount != beforeCount)
                {
                    FailClosed(
                        "The staging lifecycle exercise changed the desktop count from " +
                        beforeCount + " to " + afterCount + ".");
                    return false;
                }

                _logger.Info(
                    "Staging desktop lifecycle exercise succeeded with CreateDesktop, SetDesktopName, GetDesktopName, GetDesktopIdByNumber, and RemoveDesktop.");
                return true;
            }
            finally
            {
                if (shouldRemoveCreatedDesktop)
                {
                    _controller.RemoveDesktop(createdDesktopNumber, removeFallbackDesktop);
                }
            }
        }

        private bool TryWaitForDesktopAssignment(
            IntPtr windowHandle,
            Guid expectedDesktopId,
            int expectedDesktopNumber,
            string phase,
            out Guid observedDesktopId,
            out int observedDesktopNumber)
        {
            observedDesktopId = Guid.Empty;
            observedDesktopNumber = -1;
            string? lastDesktopManagerError = null;

            for (var attempt = 1; attempt <= MoveVerificationMaxPolls; attempt += 1)
            {
                try
                {
                    observedDesktopId = _desktopManager!.GetWindowDesktopId(windowHandle);
                    lastDesktopManagerError = null;
                }
                catch (Exception ex)
                {
                    lastDesktopManagerError = ex.Message;
                    observedDesktopId = Guid.Empty;
                }

                observedDesktopNumber = _controller.GetWindowDesktopNumber(windowHandle);
                if (observedDesktopId == expectedDesktopId)
                {
                    if (attempt > 1)
                    {
                        _logger.Info(
                            "Move-path verification for " + phase + " settled after " +
                            attempt + " poll(s); helper desktop " +
                            observedDesktopNumber + ".");
                    }

                    return true;
                }

                if (attempt < MoveVerificationMaxPolls)
                {
                    Thread.Sleep(MoveVerificationSleepMilliseconds);
                }
            }

            if (!string.IsNullOrEmpty(lastDesktopManagerError))
            {
                _logger.Info(
                    "Move-path verification for " + phase +
                    " never observed the expected COM desktop GUID; last COM error: " +
                    lastDesktopManagerError + ".");
            }

            return false;
        }

        private bool TryAcquireMoveTarget(
            out IntPtr moveTargetHandle,
            out string moveTargetLabel)
        {
            moveTargetHandle = IntPtr.Zero;
            moveTargetLabel = string.Empty;

            var moveTargetFileName =
                "locking-glass-move-target-" + Guid.NewGuid().ToString("N") + ".txt";
            _moveTargetFilePath = Path.Combine(Path.GetTempPath(), moveTargetFileName);
            File.WriteAllText(
                _moveTargetFilePath,
                "Locking Glass move-path proof target" + Environment.NewLine);

            try
            {
                _moveTargetProcess = Process.Start(
                    new ProcessStartInfo("notepad.exe", "\"" + _moveTargetFilePath + "\"")
                    {
                        UseShellExecute = true,
                        WindowStyle = ProcessWindowStyle.Normal,
                    });
            }
            catch (Exception ex)
            {
                FailClosed(
                    "Could not launch notepad.exe for the move-path exercise: " +
                    ex.Message);
                return false;
            }

            if (_moveTargetProcess == null)
            {
                FailClosed("Could not launch notepad.exe for the move-path exercise.");
                return false;
            }

            for (var attempt = 0; attempt < 80; attempt += 1)
            {
                moveTargetHandle = FindWindowByTitleFragment(moveTargetFileName);
                if (moveTargetHandle != IntPtr.Zero)
                {
                    moveTargetLabel = "spawned notepad document window";
                    return true;
                }

                Thread.Sleep(100);
            }

            FailClosed(
                "Launched notepad.exe but could not find the disposable document window by " +
                "title for the move-path exercise.");
            return false;
        }

        private static IntPtr FindWindowByTitleFragment(string titleFragment)
        {
            IntPtr foundHandle = IntPtr.Zero;
            EnumWindows(
                delegate(IntPtr windowHandle, IntPtr parameter)
                {
                    if (!IsWindowVisible(windowHandle))
                    {
                        return true;
                    }

                    var titleLength = GetWindowTextLength(windowHandle);
                    if (titleLength <= 0)
                    {
                        return true;
                    }

                    var titleBuilder = new StringBuilder(titleLength + 1);
                    GetWindowText(windowHandle, titleBuilder, titleBuilder.Capacity);
                    if (titleBuilder.ToString().IndexOf(
                            titleFragment,
                            StringComparison.OrdinalIgnoreCase) >= 0)
                    {
                        foundHandle = windowHandle;
                        return false;
                    }

                    return true;
                },
                IntPtr.Zero);

            return foundHandle;
        }

        private void ScheduleDesktopSwitch(int desktopNumber, int delayMilliseconds)
        {
            _scheduledDesktopNumber = desktopNumber;
            KillTimer(_windowHandle, SwitchTimerId);
            if (SetTimer(
                    _windowHandle,
                    SwitchTimerId,
                    (uint)delayMilliseconds,
                    IntPtr.Zero) == UIntPtr.Zero)
            {
                FailClosed(
                    "SetTimer failed for the scheduled desktop switch, so the probe could not " +
                    "exercise the live notification source.");
                return;
            }

            _logger.Info(
                "Scheduled GoToDesktopNumber(" + desktopNumber + ") in " +
                delayMilliseconds + " ms to produce a live switch event.");
        }

        private void EmitWatchReady(
            int desktopCount,
            int currentDesktopNumber,
            Guid currentDesktopId,
            string currentDesktopName)
        {
            Console.WriteLine(
                "watch-ready\t" +
                desktopCount + "\t" +
                currentDesktopNumber + "\t" +
                currentDesktopId + "\t" +
                SanitizeWatchField(currentDesktopName));
        }

        private void EmitWatchEvent(
            int oldDesktop,
            Guid oldDesktopId,
            string oldDesktopName,
            int newDesktop,
            Guid newDesktopId,
            string newDesktopName)
        {
            Console.WriteLine(
                "watch-event\t" +
                oldDesktop + "\t" +
                oldDesktopId + "\t" +
                SanitizeWatchField(oldDesktopName) + "\t" +
                newDesktop + "\t" +
                newDesktopId + "\t" +
                SanitizeWatchField(newDesktopName));
        }

        private static string SanitizeWatchField(string value)
        {
            return value
                .Replace('\t', ' ')
                .Replace('\r', ' ')
                .Replace('\n', ' ');
        }

        private void IssueDesktopSwitch(int desktopNumber)
        {
            _logger.Info("Issuing GoToDesktopNumber(" + desktopNumber + ").");
            var result = _controller.GoToDesktopNumber(desktopNumber);
            if (result < 0)
            {
                FailClosed(
                    "GoToDesktopNumber(" + desktopNumber + ") failed, so the probe could not " +
                    "exercise the live notification source.");
            }
        }

        private void CleanupHook()
        {
            KillTimer(_windowHandle, TimeoutTimerId);
            KillTimer(_windowHandle, SwitchTimerId);

            if (_hookRegistered)
            {
                var unregisterResult =
                    _controller.UnregisterPostMessageHook(_windowHandle);
                _logger.Info(
                    unregisterResult < 0
                        ? "UnregisterPostMessageHook failed during shutdown."
                        : "Unregistered VirtualDesktopAccessor post-message hook.");
                _hookRegistered = false;
            }

            if (_moveTargetProcess != null)
            {
                try
                {
                    if (!_moveTargetProcess.HasExited)
                    {
                        _moveTargetProcess.Kill();
                        _moveTargetProcess.WaitForExit(2000);
                    }
                }
                catch
                {
                    // Best-effort cleanup for the disposable move-path target.
                }
            }

            if (!string.IsNullOrEmpty(_moveTargetFilePath))
            {
                try
                {
                    if (File.Exists(_moveTargetFilePath))
                    {
                        File.Delete(_moveTargetFilePath);
                    }
                }
                catch
                {
                    // Best-effort cleanup for the disposable move-path target.
                }
            }
        }

        private void FailClosed(string detail)
        {
            if (_finished)
            {
                return;
            }

            _finished = true;
            _logger.Info("FAIL-CLOSED: " + detail);
            ExitCode = 1;
            DestroyWindow(_windowHandle);
        }

        private void Succeed(string detail)
        {
            if (_finished)
            {
                return;
            }

            _finished = true;
            _logger.Info("SUCCESS: " + detail);
            ExitCode = 0;
            DestroyWindow(_windowHandle);
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct WNDCLASSEX
        {
            public uint cbSize;
            public uint style;
            public WndProc lpfnWndProc;
            public int cbClsExtra;
            public int cbWndExtra;
            public IntPtr hInstance;
            public IntPtr hIcon;
            public IntPtr hCursor;
            public IntPtr hbrBackground;
            [MarshalAs(UnmanagedType.LPWStr)]
            public string lpszMenuName;
            [MarshalAs(UnmanagedType.LPWStr)]
            public string lpszClassName;
            public IntPtr hIconSm;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct POINT
        {
            public int X;
            public int Y;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct MSG
        {
            public IntPtr hwnd;
            public uint message;
            public IntPtr wParam;
            public IntPtr lParam;
            public uint time;
            public POINT pt;
        }

        private delegate IntPtr WndProc(
            IntPtr hwnd,
            uint message,
            IntPtr wParam,
            IntPtr lParam);

        private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr parameter);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr GetModuleHandle(string? lpModuleName);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern ushort RegisterClassEx(ref WNDCLASSEX windowClass);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool IsWindowVisible(IntPtr hwnd);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern int GetWindowTextLength(IntPtr hwnd);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern int GetWindowText(
            IntPtr hwnd,
            StringBuilder text,
            int maximumCount);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateWindowEx(
            uint exStyle,
            string className,
            string windowName,
            uint style,
            int x,
            int y,
            int width,
            int height,
            IntPtr parentHandle,
            IntPtr menuHandle,
            IntPtr instanceHandle,
            IntPtr parameter);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool ShowWindow(IntPtr hwnd, int commandShow);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool UpdateWindow(IntPtr hwnd);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr DefWindowProc(
            IntPtr hwnd,
            uint message,
            IntPtr wParam,
            IntPtr lParam);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern int GetMessage(
            out MSG message,
            IntPtr hwnd,
            uint minimumFilter,
            uint maximumFilter);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool TranslateMessage(ref MSG message);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr DispatchMessage(ref MSG message);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern UIntPtr SetTimer(
            IntPtr hwnd,
            uint eventId,
            uint elapsedMilliseconds,
            IntPtr timerProc);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool KillTimer(IntPtr hwnd, uint eventId);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool DestroyWindow(IntPtr hwnd);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern void PostQuitMessage(int exitCode);
    }
}
