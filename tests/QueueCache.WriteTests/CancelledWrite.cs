using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

// Dedicated thread: cancellation cannot accidentally target unrelated thread-pool I/O.
internal sealed class CancelledWrite : IDisposable
{
    private readonly ManualResetEventSlim ready = new(false), done = new(false);
    private readonly Thread thread;
    private SafeWaitHandle? threadHandle;
    private Exception? failure;

    public CancelledWrite(Action write)
    {
        thread = new Thread(() =>
        {
            try
            {
                threadHandle = OpenThread(1, false, GetCurrentThreadId()); // THREAD_TERMINATE required by CancelSynchronousIo.
                if (threadHandle.IsInvalid) throw new Win32Exception();
                ready.Set();
                write();
            }
            catch (Exception ex) { failure = ex; }
            finally { ready.Set(); done.Set(); }
        });
        thread.Start();
        ready.Wait();
    }

    public void CancelAndVerify()
    {
        var watch = Stopwatch.StartNew();
        var requested = false;
        while (!done.IsSet && watch.Elapsed < TimeSpan.FromSeconds(2))
        {
            if (threadHandle is not null && CancelSynchronousIo(threadHandle)) { requested = true; break; }
            var error = Marshal.GetLastWin32Error();
            if (error != 1168) throw new Win32Exception(error); // ERROR_NOT_FOUND: WriteFile has not entered kernel yet.
            Thread.Sleep(5);
        }
        if (!done.Wait(TimeSpan.FromSeconds(5))) throw new IOException("Cancellation did not complete within five seconds.");
        thread.Join();
        if (!requested || failure is not Win32Exception { NativeErrorCode: 995 })
            throw new IOException("Expected ERROR_OPERATION_ABORTED from cancelled write.", failure);
    }

    public void Dispose()
    {
        // Keep disk/buffer and native thread handle alive until the original WriteFile really completes.
        // Cancellation API success alone is not completion.
        thread.Join();
        threadHandle?.Dispose(); ready.Dispose(); done.Dispose();
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern SafeWaitHandle OpenThread(uint access, [MarshalAs(UnmanagedType.Bool)] bool inherit, uint id);
    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CancelSynchronousIo(SafeWaitHandle thread);
}
