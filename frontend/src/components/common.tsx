import { motion } from 'motion/react'
import type { ReactNode } from 'react'
import { cn } from '@/lib/utils'
import { Tooltip, TooltipContent, TooltipTrigger } from '@/components/ui/tooltip'
import type { RunState } from '@/lib/types'

/**
 * The layout's container: DESIGN.md feature card. White paper, 12px radius,
 * 1px Cloud Mist border, one soft blue-tinted shadow; Jersey 10 heading.
 */
export function Section({
  eyebrow, title, action, children, className, delay = 0,
}: {
  eyebrow?: ReactNode
  title?: ReactNode
  action?: ReactNode
  children: ReactNode
  className?: string
  delay?: number
}) {
  return (
    <motion.section
      initial={{ opacity: 0, y: 8 }}
      animate={{ opacity: 1, y: 0 }}
      transition={{ duration: 0.35, delay, ease: [0.2, 0.7, 0.2, 1] }}
      className={cn('card-paper flex min-w-0 flex-col gap-5 p-6', className)}
    >
      {(eyebrow || title || action) && (
        <header className="flex flex-wrap items-end justify-between gap-x-6 gap-y-2">
          <div className="min-w-0">
            {eyebrow && <div className="eyebrow mb-1">{eyebrow}</div>}
            {title && <h2 className="font-display text-[36px] leading-[1.11] text-slate">{title}</h2>}
          </div>
          {action}
        </header>
      )}
      {children}
    </motion.section>
  )
}

/** Label over value + unit: the basic measured-value cell. */
export function Stat({
  label, value, unit, hint, tone, className, size = 'md',
}: {
  label: ReactNode
  value: ReactNode
  unit?: string
  hint?: string
  tone?: 'good' | 'warn' | 'bad' | 'muted'
  className?: string
  size?: 'sm' | 'md' | 'lg'
}) {
  const body = (
    <div className={cn('flex min-w-0 flex-col gap-1.5', className)}>
      <span className={cn('eyebrow truncate', hint && 'decoration-stone decoration-dotted underline-offset-4 [text-decoration-line:underline]')}>{label}</span>
      <span
        className={cn(
          'num truncate leading-none',
          size === 'sm' && 'text-base',
          size === 'md' && 'text-xl',
          size === 'lg' && 'text-3xl',
          tone === 'good' && 'text-good',
          tone === 'warn' && 'text-warn',
          tone === 'bad' && 'text-clay-deep',
          tone === 'muted' && 'text-muted-foreground',
        )}
      >
        {value}
        {unit && <span className="ml-1 font-sans text-[11px] font-medium text-muted-foreground">{unit}</span>}
      </span>
    </div>
  )
  if (!hint) return body
  return (
    <Tooltip>
      <TooltipTrigger asChild><div className="cursor-help">{body}</div></TooltipTrigger>
      <TooltipContent className="max-w-64">{hint}</TooltipContent>
    </Tooltip>
  )
}

/** A row of stats separated by vertical hairlines instead of boxes. */
export function StatRow({ children, className }: { children: ReactNode; className?: string }) {
  return (
    <div className={cn('grid gap-y-5 [&>*]:border-stone [&>*:not(:first-child)]:sm:border-l [&>*:not(:first-child)]:sm:pl-5', className)}>
      {children}
    </div>
  )
}

const STATE_LABEL: Record<string, string> = {
  idle: 'Idle', align: 'Aligning', run: 'Running', raw: 'Duty test', rl: 'Measuring R/L', foc: 'FOC',
}

/** Run state as a typographic label with a live dot (no pill). */
export function StateBadge({ st, label, className }: { st: RunState | undefined; label?: string; className?: string }) {
  const live = st && st !== 'idle'
  return (
    <span className={cn('inline-flex items-center gap-2 font-sans text-[12px] font-semibold tracking-[0.06em] uppercase', className)}>
      <span className="relative flex size-2">
        {live && <span className="absolute inline-flex size-full animate-ping rounded-full bg-clay opacity-60" />}
        <span className={cn('relative inline-flex size-2 rounded-full', live ? 'bg-clay' : 'bg-cloud')} />
      </span>
      <motion.span
        key={label ?? st ?? 'none'}
        initial={{ opacity: 0.2, y: 3 }}
        animate={{ opacity: 1, y: 0 }}
        transition={{ duration: 0.18 }}
        className={live ? 'text-slate' : 'text-muted-foreground'}
      >
        {label ?? (st ? STATE_LABEL[st] ?? st : 'No data')}
      </motion.span>
    </span>
  )
}

export function SafetyBadge({ level }: { level: 'safe' | 'twitch' | 'motor' }) {
  const m = {
    safe: ['No motor current', 'text-muted-foreground', 'bg-cloud'],
    twitch: ['Rotor moves', 'text-slate', 'bg-warn'],
    motor: ['Motor turns', 'text-slate font-semibold', 'bg-clay-deep'],
  }[level]
  return (
    <span className={cn('inline-flex items-center gap-1.5 font-sans text-[11px] whitespace-nowrap', m[1])}>
      <span className={cn('size-1.5 rounded-full', m[2])} />{m[0]}
    </span>
  )
}

/**
 * Linear meter with marker lines (limit, trip). Built on the same idea as
 * shadcn Progress, which has no markers.
 */
export function Meter({
  value, max, marks = [], color = 'var(--color-slate)',
}: {
  value: number
  max: number
  marks?: { at: number; label: string; strong?: boolean }[]
  color?: string
}) {
  const pct = (v: number) => `${Math.max(0, Math.min(100, (v / max) * 100))}%`
  return (
    <div className="relative pt-4">
      <div className="relative h-1.5 w-full overflow-hidden rounded-[2px] bg-oat">
        <motion.div className="absolute inset-y-0 left-0 rounded-[2px]" style={{ background: color }}
          animate={{ width: pct(value) }} transition={{ type: 'spring', stiffness: 240, damping: 32 }} />
      </div>
      {marks.map((m) => (
        <div key={m.label} className="absolute top-0 bottom-0 flex flex-col items-center" style={{ left: pct(m.at) }}>
          <span className={cn('num -translate-x-1/2 text-[10px] whitespace-nowrap', m.strong ? 'text-slate' : 'text-muted-foreground')}>{m.label}</span>
          <span className={cn('absolute top-3.5 h-3.5 w-px -translate-x-1/2', m.strong ? 'bg-slate' : 'bg-cloud-dark')} />
        </div>
      ))}
    </div>
  )
}

export const fmt = (v: number | undefined | null, d = 2) =>
  v === undefined || v === null || Number.isNaN(v) ? '—' : v.toFixed(d)

export const PHASES = [
  { key: 0, name: 'A', color: 'var(--color-phase-a)' },
  { key: 1, name: 'B', color: 'var(--color-phase-b)' },
  { key: 2, name: 'C', color: 'var(--color-phase-c)' },
] as const

// kept for older imports
export const SectionTitle = ({ eyebrow, title, children }: { eyebrow?: string; title: string; children?: ReactNode }) => (
  <div className="flex flex-wrap items-end justify-between gap-4">
    <div>
      {eyebrow && <div className="eyebrow mb-1">{eyebrow}</div>}
      <h1 className="font-display text-[60px] leading-[0.85] text-slate">{title}</h1>
    </div>
    {children}
  </div>
)

// ---- 8-bit pixel cloud (DESIGN.md: Pixel Cloud Sprite) ----------------------
// Hand-authored on a 22x10 grid: flat fills, 1px Slate Pencil outline,
// no anti-aliasing. # outline, w white, m Cloud Mist, s shade.
const CLOUD = [
  '........#####.........',
  '......##wwwww##.......',
  '.....#wwwwwwwww#.###..',
  '...##wwwwwwwwwww#www#.',
  '..#wwwwwwwwwwwwwwwwww#',
  '.#wwwwwwwwwwwwwwwwwmm#',
  '#wwwwwwwwwwwwwwwwmmmm#',
  '#mmwwwwwwwwwwwmmmmsss#',
  '.#ssmmmmmmmmmmmsssss#.',
  '..##################..',
]
const CLOUD_FILL: Record<string, string> = { '#': '#5c6066', w: '#ffffff', m: '#e3f1fe', s: '#c9def3' }

/** Floating pixel cloud. Decoration only: keep it off-centre and off text. */
export function PixelCloud({ px = 4, className, drift = 6, delay = 0 }: {
  px?: number; className?: string; drift?: number; delay?: number
}) {
  return (
    <motion.svg
      aria-hidden
      width={22 * px}
      height={10 * px}
      viewBox="0 0 22 10"
      className={cn('pixelated pointer-events-none select-none', className)}
      animate={{ x: [0, drift, 0] }}
      transition={{ duration: 9, repeat: Infinity, ease: 'easeInOut', delay }}
    >
      {CLOUD.flatMap((row, y) => [...row].map((c, x) =>
        c === '.' ? null : <rect key={`${x}-${y}`} x={x} y={y} width={1.02} height={1.02} fill={CLOUD_FILL[c]} />))}
    </motion.svg>
  )
}
