import { motion } from 'motion/react'
import type { Tel } from '@/lib/types'
import { PHASES } from '../common'

// Custom SVG: no component library has a space-vector / rotor-angle dial.
// Shows the three phase axes, the commanded electrical angle (needle), the
// measured current vector (alpha/beta from iA, iB) and, in 6-step, the active
// 60° sector.

const R = 100
const deg = (a: number) => (a * Math.PI) / 180
// math angle (CCW, 0 = phase A axis to the right) -> SVG point
const pt = (aDeg: number, r: number) => [R + r * Math.cos(deg(aDeg)), R - r * Math.sin(deg(aDeg))] as const

export function RotorDial({ tel, ilim, size = 220 }: { tel: Tel | null; ilim: number; size?: number }) {
  const th = tel?.th ?? 0
  const live = !!tel && tel.st !== 'idle'
  const [ia, ib] = tel?.i ?? [0, 0, 0]
  const ial = ia
  const ibe = (ia + 2 * ib) / Math.sqrt(3)
  const imag = Math.hypot(ial, ibe)
  const iang = (Math.atan2(ibe, ial) * 180) / Math.PI
  const ir = Math.min(1, imag / Math.max(0.5, ilim * 1.5)) * 78
  const sixStep = tel?.mod === '6step' && (tel.st === 'run' || tel.st === 'align')
  const sector = tel?.step ?? -1

  return (
    <svg viewBox="0 0 200 200" width={size} height={size} className="overflow-visible" role="img"
      aria-label={`Electrical angle ${th.toFixed(0)} degrees`}>
      {/* rings */}
      <circle cx={R} cy={R} r={92} fill="none" stroke="var(--color-slate)" strokeWidth={0.75} />
      <circle cx={R} cy={R} r={46} fill="none" stroke="var(--color-stone)" strokeWidth={0.6} strokeDasharray="1.5 3" />
      {Array.from({ length: 72 }, (_, k) => {
        const a = k * 5
        const major = k % 6 === 0
        const [x1, y1] = pt(a, major ? 85 : 89)
        const [x2, y2] = pt(a, 92)
        return <line key={k} x1={x1} y1={y1} x2={x2} y2={y2} stroke={major ? 'var(--color-slate)' : 'var(--color-cloud)'} strokeWidth={major ? 0.9 : 0.5} />
      })}
      {[30, 90, 150, 210, 270, 330].map((a) => {
        const [x, y] = pt(a, 77)
        return <text key={a} x={x} y={y} textAnchor="middle" dominantBaseline="central"
          className="num text-[6.5px]" fill="var(--color-cloud-dark)">{a}°</text>
      })}

      {/* 6-step: active sector (the step starts at 60*s - 30 deg of the voltage vector) */}
      {sixStep && sector >= 0 && (
        <motion.path
          key={sector}
          initial={{ opacity: 0 }}
          animate={{ opacity: 1 }}
          d={sectorPath(sector * 60, sector * 60 + 60, 92)}
          fill="var(--color-manilla)"
          stroke="none"
        />
      )}

      {/* phase axes */}
      {PHASES.map((p, k) => {
        const a = k * 120
        const [x, y] = pt(a, 92)
        const [lx, ly] = pt(a, 104)
        return (
          <g key={p.name}>
            <line x1={R} y1={R} x2={x} y2={y} stroke={p.color} strokeWidth={1} strokeOpacity={0.6} strokeDasharray="3 2" />
            <text x={lx} y={ly} textAnchor="middle" dominantBaseline="central"
              className="font-sans text-[10px] font-semibold" fill={p.color}>{p.name}</text>
          </g>
        )
      })}

      {/* current vector */}
      {live && imag > 0.05 && (
        <g>
          <line x1={R} y1={R} x2={pt(iang, ir)[0]} y2={pt(iang, ir)[1]} stroke="var(--color-cloud-dark)"
            strokeWidth={5} strokeLinecap="round" strokeOpacity={0.7} />
          <circle cx={pt(iang, ir)[0]} cy={pt(iang, ir)[1]} r={4.5} fill="var(--color-ivory)"
            stroke="var(--color-cloud-dark)" strokeWidth={2} />
        </g>
      )}

      {/* angle needle */}
      <g style={{ transform: `rotate(${-th}deg)`, transformOrigin: '100px 100px', transition: live ? 'none' : 'transform 0.6s ease' }}>
        <line x1={R} y1={R} x2={R + 88} y2={R} stroke="var(--color-slate)" strokeWidth={1.75} strokeLinecap="round" />
        <path d={`M ${R + 92} ${R} l -9 -3.5 v 7 z`} fill="var(--color-slate)" />
      </g>
      <circle cx={R} cy={R} r={3.5} fill="var(--color-slate)" />
    </svg>
  )
}

function sectorPath(a0: number, a1: number, r: number) {
  const [x0, y0] = pt(a0, r)
  const [x1, y1] = pt(a1, r)
  return `M ${R} ${R} L ${x0} ${y0} A ${r} ${r} 0 0 0 ${x1} ${y1} Z`
}
