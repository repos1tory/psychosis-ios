# Psychosis `listen` — conversational voice mode (plan)

Goal: a zen "talk to it" mode. Speech in → transcript → agent → spoken reply.
No buttons required once started. Two-tier silence timing so replies feel
quick but the system never interrupts a thinking user.

## 1. Architecture

    ┌────────────────────────── SpringBoard (Psychosis tweak) ─────────────────────────┐
    │ listen engine (background thread)                                                │
    │  mic ──► AVAudioRecorder segment per turn ──► /var/mobile/Library/PsychosisVoice/│
    │           metering poll 50ms → energy VAD                                         │
    │  state machine + events.log (JSON lines)                                          │
    └──────────────┬──────────────────────────────────────────────────────────────────┘
                   │ turn-*.wav + .done markers, events.log
    ┌──────────────▼─────────────────── node watcher (voice-pipeline.mjs) ─────────────┐
    │ watch incoming/ ──► STT adapter ──► inject into opencode (127.0.0.1:4097)        │
    │ (fallback: append to pending-messages.jsonl if injection unavailable)            │
    └──────────────────────────────────────────────────────────────────────────────────┘
    Agent replies via:  psychosis say [voice=..] "short answer"

Separation of concerns: the tweak owns hardware+audio (stable, needs respring to
change). The watcher owns STT + routing (pure JS, iterate with zero resprings).

## 2. Timing model (the core UX)

Two silence thresholds per turn:

    speech ──► [silence 0.6s] ──► PRE-PAUSE  (tight)
                    │ close segment file, watcher starts STT immediately
                    │ (transcript usually ready in ~1-3s, overlapping the hold)
                    ▼
              [silence keeps going]
                    │
                    ├─ user speaks again before HOLD expires:
                    │     cancel this turn's mining; recording continues
                    │     (the user was thinking — never interrupted)
                    │
                    ▼ [silence reaches 1.8s total] ──► HOLD expires
                          publish transcript to agent  (feels instant:
                          STT already ran during the hold)

Percept: you stop talking → reply starts ~4-8s later (STT tail + model time);
but pausing mid-thought is always safe.

Params (defaults):
    prepause_ms   600    tight: candidate end-of-turn, fires STT early
    hold_ms       1200   patient: extra silence before submitting
    min_speech_ms 300    ignore coughs/bleeps shorter than this
    max_turn_ms   30000  force-submit monologues
    idle_exit_s   180    zen mode quits after this much total silence
    barge_in      true   user speech while reply is speaking → stop the reply

Barge-in: while `say` is speaking and VAD detects speech, the tweak calls
stopSpeakingAtBoundary:Immediate and the say command returns "ok say interrupted".
User always wins.

## 3. New commands

    listen start [prepause_ms=P] [hold_ms=H] [max_turn_s=M] [idle_s=I]
    listen stop          → summary: ok listen stop turns=<n>
    listen status        → ok listen <idle|listening|speaking> turns=<n>

Files:
    /var/mobile/Library/PsychosisVoice/
        incoming/turn-<epoch>.wav (+ .done)   completed turns, FIFO
        incoming/turn-<epoch>.cancelled       resumed speech → discard
        events.log                             jsonl: turn_start, speech_end,
                                               prepause, held, submitted, done
        config.json                            watcher config (STT engine etc.)

## 4. STT adapter (decision still open)

Interface: transcribe(wavPath) → text

    A. Apple Speech (SFSpeechRecognizer)  — instant, offline, FREE, but must run
                                            inside the tweak (TCC risk on speech
                                            recognition permission)
    B. Whisper API (cloud)                — best accuracy, trivial from node,
                                            needs API key, ~1-3s/short clip
    C. whisper.cpp on-device              — offline, no keys; A9 CPU with
                                            tiny.en: marginal for realtime;
                                            we have a working on-device clang

Plan: build M2 with the adapter seam and a stub; wire the chosen engine then.
Recommendation: B if a key is available, else try A (same TCC context that
record already proved), C as the cool endgame.

## 5. Milestones

    M1  tweak `listen` engine: VAD, segments, events.log, listen start/stop/status
        testable alone: speak → turn-*.wav files appear, cancelled on resume
    M2  watcher skeleton: file watch → pluggable STT (stub) → pending-messages.jsonl
    M3  STT wiring (per decision) + injection into opencode session
    M4  reply conventions ("reply short, it will be spoken") + barge-in + idle exit
    M5  share the engine with the walkie-talkie (volume-button trigger) and,
        later, a LensChat mic button hitting a tiny HTTP endpoint on :6002

## 6. Latency budget (target)

    stop talking       0 s
    prepause fires     0.6 s   (file closed, STT starts)
    hold expires       1.8 s   (transcript usually ready or nearly)
    model reply        2-10 s  (policy: short spoken replies)
    say starts         ~4-12 s ▸ conversational if replies stay short

## 7. Risks / notes

    - orange mic dot stays on while listening (inherent; that's the trade)
    - metering granularity ~50-100ms: fine for this purpose
    - long mic sessions in SpringBoard: record already proved TCC works; watch
      jetsam headroom in long sessions
    - opencode message injection needs API mapping (server on 127.0.0.1:4097)
      — check with the opencode skill before M3
    - agent latency dominates; keep replies short by convention in the
      injected message preamble
