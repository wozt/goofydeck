# Sonic Pi SFX: "water drop" attempt for TicTacToe O placement.
#
# How to use:
# 1) Launch Sonic Pi.
# 2) Click "REC" (record).
# 3) Run this buffer once (press Run).
# 4) Stop recording -> pick the generated wav in ~/.sonic-pi/recordings/
# 5) Convert/copy to:
#    mymedia/miniapp/.assets/tictactoe/o_drop.wav
#
# Tip: you can run it multiple times while recording to compare (it will make multiple drops).

use_debug false
use_bpm 60

define :drop do |base_note: :e5, tail_cutoff: 110|
  # "plop" transient: fast pitch sweep down
  with_fx :reverb, mix: 0.22, room: 0.4 do
    with_fx :echo, mix: 0.18, phase: 0.025, decay: 1.2 do
      synth :sine, note: base_note, attack: 0.001, decay: 0.035, release: 0.01, amp: 0.9
      sleep 0.012
      synth :sine, note: (note(base_note) - 12), attack: 0.001, decay: 0.045, release: 0.01, amp: 0.55
    end
  end

  # "wet" tail: filtered noise + short pluck resonance
  with_fx :hpf, cutoff: 70 do
    with_fx :lpf, cutoff: tail_cutoff do
      synth :bnoise, attack: 0.0, decay: 0.09, release: 0.06, amp: 0.20
    end
  end

  with_fx :lpf, cutoff: 95 do
    synth :pluck, note: (note(base_note) - 19), attack: 0.0, decay: 0.08, release: 0.02, amp: 0.35
  end
end

drop base_note: :f5, tail_cutoff: 115
sleep 0.22

