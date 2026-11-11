import React from 'react';
import {AbsoluteFill, Easing, interpolate, spring, useCurrentFrame, useVideoConfig} from 'remotion';

// Flicks: frame each one starts, finger direction (-1 = fingers move left,
// which pulls the next window in from the right), and the tape column after it.
const FLICKS = [
  {at: 28, dir: -1},
  {at: 59, dir: -1},
  {at: 90, dir: -1},
  {at: 127, dir: -1}, // last window already in view: boing
  {at: 164, dir: 1},
  {at: 189, dir: 1},
  {at: 214, dir: 1},
  {at: 248, dir: 1}, // first window already in view: boing
];
export const DURATION = 310;

const CAPTIONS = [
  {from: 0, to: 32, text: 'Flick two fingers sideways'},
  {from: 32, to: 78, text: 'The view moves. Focus stays put.'},
  {from: 78, to: 121, text: 'One flick, one window'},
  {from: 121, to: 160, text: 'Boing: no more windows that way'},
  {from: 160, to: 242, text: 'Flick back the other way'},
  {from: 242, to: 276, text: 'Boing at both ends'},
  {from: 276, to: DURATION, text: 'hyprflick  ·  a Hyprland plugin'},
];

const C = {
  bg: '#0c0e13',
  bezel: '#181b22',
  screen: '#0f1218',
  win: '#151922',
  winBorder: '#272c37',
  focus: '#7aa2f7',
  pad: '#151820',
  padBorder: '#262b35',
  text: '#e6e9ef',
  dim: '#7c8496',
};
const ACCENTS = ['#7aa2f7', '#9ece6a', '#e0af68', '#bb9af7', '#7dcfff'];

// Screen geometry
const SCREEN = {x: 200, y: 96, w: 880, h: 420, pad: 20};
const VIEW_W = SCREEN.w - SCREEN.pad * 2;
const VIEW_H = SCREEN.h - SCREEN.pad * 2;
const GAP = 12;
// Like scrolling:column_width = 0.475, each column box is a bit under half the
// screen, so a sliver of the neighbouring window always shows.
const PITCH = VIEW_W * 0.475;
const COL_W = PITCH - GAP;
const N = 5;

// Same rule as the plugin: forward lines up the first column sticking out on
// the right with the right edge; backward lines up the first column hidden on
// the left with the left edge; clamp to the strip.
function nextOffset(o: number, forward: boolean) {
  const max = N * PITCH - VIEW_W;
  if (forward) {
    for (let i = 0; i < N; i++) if ((i + 1) * PITCH > o + VIEW_W + 1) return Math.min((i + 1) * PITCH - VIEW_W, max);
  } else {
    for (let i = N - 1; i >= 0; i--) if (i * PITCH < o - 1) return Math.max(i * PITCH, 0);
  }
  return o;
}
const OFFSETS = (() => {
  const out: number[] = [];
  let o = 0;
  for (const f of FLICKS) out.push((o = nextOffset(o, f.dir < 0)));
  return out;
})();
const FOCUSED = 1;

// Touchpad geometry
const PAD = {w: 440, h: 230, y: 560};
const PAD_X = (1280 - PAD.w) / 2;
const FINGER_R = 24;
const TRAVEL = 120;
const FLICK_LEN = 24;

const ease = Easing.bezier(0.2, 0.7, 0.2, 1);

const BOING = 30;

function useTapeOffset(frame: number, fps: number) {
  let o = 0;
  let prevTarget = 0;
  let boing = 0;
  FLICKS.forEach((f, k) => {
    const target = OFFSETS[k];
    const start = f.at + 7;
    const same = target === prevTarget;
    prevTarget = target;
    if (same) {
      // Nudge past the end, hold ~120 ms, spring back.
      const out = spring({frame: frame - start, fps, config: {damping: 14, stiffness: 260, mass: 0.6}});
      const back = spring({frame: frame - start - 4, fps, config: {damping: 12, stiffness: 180, mass: 0.7}});
      boing += -f.dir * BOING * (out - back);
      return;
    }
    const s = spring({frame: frame - start, fps, config: {damping: 200, stiffness: 200, mass: 0.7}});
    o = o + (target - o) * s;
  });
  return o + boing;
}

const Window: React.FC<{i: number; x: number}> = ({i, x}) => {
  const accent = ACCENTS[i % ACCENTS.length];
  const focused = i === FOCUSED;
  const bars = [0.72, 0.5, 0.86, 0.4, 0.64, 0.78, 0.3, 0.58];
  return (
    <div
      style={{
        position: 'absolute',
        left: x,
        top: GAP,
        width: COL_W,
        height: VIEW_H - GAP * 2,
        borderRadius: 12,
        background: C.win,
        border: `2px solid ${focused ? C.focus : C.winBorder}`,
        boxShadow: focused ? `0 0 24px ${C.focus}33` : 'none',
        overflow: 'hidden',
        boxSizing: 'border-box',
      }}
    >
      <div style={{height: 30, display: 'flex', alignItems: 'center', gap: 7, paddingLeft: 14, borderBottom: `1px solid ${C.winBorder}`}}>
        {[0, 1, 2].map((d) => (
          <div key={d} style={{width: 10, height: 10, borderRadius: 5, background: C.winBorder}} />
        ))}
      </div>
      <div style={{padding: 22, display: 'flex', flexDirection: 'column', gap: 16}}>
        <div style={{width: '45%', height: 14, borderRadius: 7, background: accent, opacity: 0.85}} />
        {bars.map((w, k) => (
          <div
            key={k}
            style={{
              width: `${((w + i * 0.13 + k * 0.07) % 0.6) * 100 + 30}%`,
              height: 10,
              borderRadius: 5,
              background: k % 3 === 2 ? accent : '#2a303c',
              opacity: k % 3 === 2 ? 0.45 : 1,
            }}
          />
        ))}
      </div>
    </div>
  );
};

const Fingers: React.FC<{frame: number}> = ({frame}) => {
  const active = FLICKS.find((f) => frame >= f.at && frame < f.at + FLICK_LEN);
  if (!active) return null;
  const t = frame - active.at;
  const cx = PAD.w / 2;
  const cy = PAD.h / 2;

  const pos = (tt: number) =>
    interpolate(tt, [2, 14], [-active.dir * TRAVEL * 0.5, active.dir * TRAVEL * 0.5], {
      extrapolateLeft: 'clamp',
      extrapolateRight: 'clamp',
      easing: ease,
    });
  const opacity = interpolate(t, [0, 3, 15, FLICK_LEN], [0, 1, 1, 0], {extrapolateRight: 'clamp'});
  const press = interpolate(t, [0, 3, 15, FLICK_LEN], [1.25, 1, 1, 1.2], {extrapolateRight: 'clamp'});

  const tips = [
    {dx: -34, dy: 8},
    {dx: 34, dy: -6},
  ];

  return (
    <>
      {[4, 3, 2, 1].map((lag) => (
        <React.Fragment key={lag}>
          {tips.map((tip, k) => (
            <div
              key={k}
              style={{
                position: 'absolute',
                left: cx + tip.dx + pos(t - lag) - FINGER_R,
                top: cy + tip.dy - FINGER_R,
                width: FINGER_R * 2,
                height: FINGER_R * 2,
                borderRadius: FINGER_R,
                background: '#ffffff',
                opacity: opacity * 0.06 * (5 - lag),
              }}
            />
          ))}
        </React.Fragment>
      ))}
      {tips.map((tip, k) => (
        <div
          key={k}
          style={{
            position: 'absolute',
            left: cx + tip.dx + pos(t) - FINGER_R,
            top: cy + tip.dy - FINGER_R,
            width: FINGER_R * 2,
            height: FINGER_R * 2,
            borderRadius: FINGER_R,
            background: 'radial-gradient(circle at 40% 35%, #ffffff, #c9d2e3)',
            boxShadow: '0 0 30px rgba(255,255,255,0.35)',
            opacity,
            transform: `scale(${press})`,
          }}
        />
      ))}
    </>
  );
};

const Caption: React.FC<{frame: number}> = ({frame}) => {
  const c = CAPTIONS.find((x) => frame >= x.from && frame < x.to)!;
  const opacity = c.from === 0 ? interpolate(frame, [c.to - 8, c.to], [1, 0], {extrapolateLeft: 'clamp'}) : interpolate(frame, [c.from, c.from + 10, c.to - 8, c.to], [0, 1, 1, c.to === DURATION ? 1 : 0]);
  const y = c.from === 0 ? 0 : interpolate(frame, [c.from, c.from + 10], [8, 0], {extrapolateRight: 'clamp'});
  return (
    <div
      style={{
        position: 'absolute',
        top: 30,
        width: '100%',
        textAlign: 'center',
        fontSize: 34,
        fontWeight: 600,
        letterSpacing: -0.5,
        color: C.text,
        opacity,
        transform: `translateY(${y}px)`,
      }}
    >
      {c.text}
    </div>
  );
};

export const Demo: React.FC = () => {
  const frame = useCurrentFrame();
  const {fps} = useVideoConfig();
  const offset = useTapeOffset(frame, fps);

  return (
    <AbsoluteFill style={{background: `radial-gradient(ellipse at 50% 30%, #161a23 0%, ${C.bg} 70%)`, fontFamily: 'Inter, "Helvetica Neue", Arial, sans-serif'}}>
      <Caption frame={frame} />

      {/* Screen */}
      <div style={{position: 'absolute', left: SCREEN.x, top: SCREEN.y, width: SCREEN.w, height: SCREEN.h, borderRadius: 22, background: C.bezel, boxShadow: '0 30px 80px rgba(0,0,0,0.5)'}}>
        <div style={{position: 'absolute', left: SCREEN.pad, top: SCREEN.pad, width: VIEW_W, height: VIEW_H, overflow: 'hidden', borderRadius: 12, background: C.screen}}>
          <div style={{position: 'absolute', left: 0, top: 0, transform: `translateX(${-offset}px)`}}>
            {[0, 1, 2, 3, 4].map((i) => (
              <Window key={i} i={i} x={i * PITCH + GAP / 2} />
            ))}
          </div>
        </div>
      </div>

      {/* Tape position dots */}
      <div style={{position: 'absolute', top: SCREEN.y + SCREEN.h + 14, width: '100%', display: 'flex', justifyContent: 'center', gap: 10}}>
        {[0, 1, 2, 3, 4].map((i) => {
          const mid = i * PITCH + PITCH / 2;
          const visible = mid > offset && mid < offset + VIEW_W;
          return <div key={i} style={{width: 8, height: 8, borderRadius: 4, background: i === FOCUSED ? C.focus : C.dim, opacity: visible ? 1 : 0.3}} />;
        })}
      </div>

      {/* Touchpad */}
      <div style={{position: 'absolute', left: PAD_X, top: PAD.y, width: PAD.w, height: PAD.h, borderRadius: 24, background: C.pad, border: `2px solid ${C.padBorder}`, overflow: 'hidden', boxSizing: 'border-box'}}>
        <Fingers frame={frame} />
      </div>

      <div style={{position: 'absolute', bottom: 18, width: '100%', textAlign: 'center', fontSize: 18, color: C.dim, fontFamily: '"JetBrainsMono Nerd Font", monospace'}}>
        github.com/finereli/hyprflick
      </div>
    </AbsoluteFill>
  );
};
