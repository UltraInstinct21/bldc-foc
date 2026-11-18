import { motion } from 'motion/react'
import type { Tel } from '@/lib/types'
import { cn } from '@/lib/utils'
import { PHASES } from '../common'

// Custom: bridge outputs per phase. Track = duty written to TIM1 (0-100 %),
// tick at 50 %. In 6-step the floating phase is shown explicitly.
const STEPS = [
  ['P', 'L', 'Z'], ['P', 'Z', 'L'], ['Z', 'P', 'L'], ['L', 'P', 'Z'], ['L', 'Z', 'P'], ['Z', 'L', 'P'],
] as const

export function PhaseBars({ tel }: { tel: Tel | null }) {
  const live = !!tel && tel.st !== 'idle'
  const six = live && tel!.mod === '6step' && (tel!.st === 'run' || tel!.st === 'align') && tel!.step >= 0
  return (
    <div className="flex flex-col gap-5">
      {PHASES.map((p, k) => {
        const d = live ? tel!.d[k] * 100 : 0
        const st = six ? STEPS[tel!.step][k] : null
        const state = !live ? 'Hi-Z' : st === 'Z' ? 'floating' : st === 'L' ? 'low side on' : 'switching'
        return (
          <div key={p.name} className="grid grid-cols-[2.5rem_minmax(0,1fr)_5.5rem] items-center gap-4">
            <span className="font-sans text-[13px] font-semibold" style={{ color: p.color }}>{p.name}</span>
            <div className="relative h-2 rounded-[2px] bg-oat">
              <span className="absolute inset-y-[-3px] left-1/2 w-px bg-cloud" />
              <motion.div
                className="absolute inset-y-0 left-0 rounded-[2px]"
                style={{ background: p.color }}
                animate={{ width: `${Math.max(0, Math.min(100, d))}%`, opacity: st === 'Z' ? 0.25 : 1 }}
                transition={{ duration: 0.08, ease: 'linear' }}
              />
            </div>
            <div className="text-right leading-tight">
              <div className={cn('num text-[15px]', !live && 'text-muted-foreground')}>{d.toFixed(1)}<span className="ml-0.5 text-[11px] text-muted-foreground">%</span></div>
              <div className="font-sans text-[10.5px] text-muted-foreground">{state}</div>
            </div>
          </div>
        )
      })}
    </div>
  )
}
