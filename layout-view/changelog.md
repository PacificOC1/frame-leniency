# v0.2.0
- Layout copies play the same as the original: decoration and No Touch objects that a trigger uses the position of (rotate centre, move / teleport target, follow group, camera target...) or that are group parents now stay, hidden, instead of being dropped.
- Copies leave out more: unused triggers (move, spawn, toggle, rotate, follow, count... with nothing left in their target group), alpha and fade triggers, stacked duplicate blocks / spikes / slopes, and more visual triggers (ghost trail, show/hide player, BG / MG speed, edit middleground, area / enter tint, camera and BPM guides). Each has its own setting.
- Collision state blocks and toggle blocks are always kept.
- Layout copies of your own uploaded levels are allowed.
- Shader skipping only happens when it's safe, so it can't give a black screen.

# v0.1.0
- Initial release: layout view while playing, and layout copies.
