package top.mobilegl.plugin.trace;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executor;
import java.util.concurrent.TimeUnit;

/** Deterministic Activity replacement while native work or its completion is pending. */
public final class TraceReplaySessionTest {
    private static final class Queue implements Executor {
        final ArrayDeque<Runnable> commands = new ArrayDeque<>();

        @Override
        public void execute(Runnable command) {
            commands.add(command);
        }

        void drain() {
            while (!commands.isEmpty()) {
                commands.remove().run();
            }
        }
    }

    private static void require(boolean condition, String message) {
        if (!condition) {
            throw new AssertionError(message);
        }
    }

    public static void main(String[] args) throws InterruptedException {
        Queue worker = new Queue();
        Queue main = new Queue();
        TraceReplaySession<String> session = new TraceReplaySession<>(worker, main);
        List<String> oldResults = new ArrayList<>();
        List<String> newResults = new ArrayList<>();
        TraceReplaySession.Listener<String> oldActivity = oldResults::add;
        TraceReplaySession.Listener<String> newActivity = newResults::add;
        int[] nativeCalls = {0};

        // Recreation before a valid Surface: the replacement may start the first run.
        session.attach(oldActivity);
        session.detach(oldActivity);
        session.attach(newActivity);
        require(session.start(() -> { ++nativeCalls[0]; return "real native result"; }),
                "replacement must be able to start a pending request");
        require(session.hasStarted(), "ownership must be published before worker dispatch");

        // surfaceCreated and surfaceChanged can both queue callbacks, and a recreated
        // Activity receives both again. None may dispatch another native invocation.
        session.detach(newActivity);
        session.attach(oldActivity);
        require(!session.start(() -> { ++nativeCalls[0]; return "duplicate"; }),
                "Activity recreation must retain the running invocation");
        worker.drain();
        require(nativeCalls[0] == 1, "only one worker may execute the native request");

        // The native result can arrive between the old Activity's destruction and the
        // replacement attaching. It must be kept, without calling a destroyed owner.
        session.detach(oldActivity);
        main.drain();
        require(oldResults.isEmpty() && newResults.isEmpty(), "destroyed owners must not be notified");
        session.attach(newActivity);
        require(newResults.equals(List.of("real native result")), "replacement must receive original result");
        require(!session.start(() -> "replayed completed request"), "completed request must not replay");

        // An obsolete owner must not detach the current owner, including while running.
        TraceReplaySession<String> next = new TraceReplaySession<>(worker, main);
        next.attach(oldActivity);
        next.attach(newActivity);
        next.detach(oldActivity);
        next.start(() -> "next native result");
        worker.drain();
        main.drain();
        require(oldResults.isEmpty(), "obsolete owner was notified");
        require(newResults.equals(List.of("real native result", "next native result")),
                "late detach lost the current owner");

        // Replace the observer while the worker is actually inside the native-call
        // boundary, not just queued. Latches force the interleaving without sleeps.
        CountDownLatch entered = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        List<Thread> threads = new ArrayList<>();
        TraceReplaySession<String> running = new TraceReplaySession<>(command -> {
            Thread thread = new Thread(command);
            threads.add(thread);
            thread.start();
        }, main);
        running.attach(oldActivity);
        running.start(() -> {
            entered.countDown();
            try {
                if (!release.await(5, TimeUnit.SECONDS)) {
                    throw new AssertionError("test did not release the native invocation");
                }
            } catch (InterruptedException exception) {
                throw new AssertionError(exception);
            }
            return "running native result";
        });
        require(entered.await(5, TimeUnit.SECONDS), "native worker did not enter");
        running.detach(oldActivity);
        running.attach(newActivity);
        boolean duplicate = running.start(() -> "duplicate while running");
        release.countDown();
        for (Thread thread : threads) {
            thread.join(5000);
            require(!thread.isAlive(), "native worker did not finish");
        }
        require(!duplicate, "recreation entered native code while original invocation was active");
        main.drain();
        require(oldResults.isEmpty(), "running invocation notified destroyed Activity");
        require(newResults.equals(List.of("real native result", "next native result", "running native result")),
                "running invocation lost its original result during Activity replacement");
        System.out.println("TraceReplaySession: pending, running and completed recreation passed");
    }
}
