package top.mobilegl.plugin.trace;

import java.util.concurrent.Executor;
import java.util.function.Supplier;

/** One native invocation, retained when Android replaces the Activity that displays it. */
final class TraceReplaySession<T> {
    interface Listener<T> {
        void onComplete(T result);
    }

    private final Executor worker;
    private final Executor completion;
    private boolean started;
    private boolean completed;
    private T result;
    private Listener<T> listener;

    TraceReplaySession(Executor worker, Executor completion) {
        this.worker = worker;
        this.completion = completion;
    }

    // All session state is accessed on the main thread. Only replay.get() runs on the worker.
    boolean hasStarted() {
        return started;
    }

    boolean start(Supplier<T> replay) {
        if (started) {
            return false;
        }
        started = true;
        worker.execute(() -> {
            T replayResult = replay.get();
            completion.execute(() -> {
                result = replayResult;
                completed = true;
                if (listener != null) {
                    listener.onComplete(result);
                }
            });
        });
        return true;
    }

    void attach(Listener<T> newListener) {
        listener = newListener;
        if (completed) {
            listener.onComplete(result);
        }
    }

    void detach(Listener<T> oldListener) {
        if (listener == oldListener) {
            listener = null;
        }
    }
}
