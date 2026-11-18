import { AnimatePresence, motion } from 'motion/react'
import { ChevronDown, Play, Square, Wand2 } from 'lucide-react'
import { useEffect, useMemo, useRef, useState } from 'react'
import { Button } from '@/components/ui/button'
import { Collapsible, CollapsibleContent, CollapsibleTrigger } from '@/components/ui/collapsible'
import { Input } from '@/components/ui/input'
import { Label } from '@/components/ui/label'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select'
import { Slider } from '@/components/ui/slider'
import { Switch } from '@/components/ui/switch'
import { cmd, estop, throttled } from '@/lib/api'
import { rpmOf, useStore } from '@/lib/store'
import type { Mod } from '@/lib/types'
import { cn } from '@/lib/utils'

export type DriveMode = Mod | 'if' | 'sl'

const MODES: { id: DriveMode; label: string; hint: string; group: 'Open loop' | 'Closed current loop' }[] = [
  { id: '6step', label: '6-step', hint: 'trapezoidal', group: 'Open loop' },
  { id: 'sine', label: 'Sine', hint: 'sinusoidal PWM', group: 'Open loop' },
  { id: 'svpwm', label: 'SVPWM', hint: 'space vector', group: 'Open loop' },
  { id: 'dpwm', label: 'DPWM', hint: 'bottom-clamped', group: 'Open loop' },
  { id: 'if', label: 'I/f', hint: 'current loop, open angle', group: 'Closed current loop' },
  { id: 'sl', label: 'Sensorless', hint: 'observer + PLL', group: 'Closed current loop' },
]

/** A slider bound to one console parameter. Sends live while dragging. */
function LiveSlider({
  label, value, min, max, step, unit, command, digits = 1, sub, disabled,
}: {
  label: string
  value: number | undefined
  min: number
  max: number
  step: number
  unit: string
  command: (v: number) => string
  digits?: number
  sub?: (v: number) => string
  disabled?: boolean
}) {
  const [local, setLocal] = useState<number | null>(null)
  const dragging = useRef(false)
  const send = useMemo(() => throttled(80), [])
  useEffect(() => { if (!dragging.current) setLocal(null) }, [value])
  const v = local ?? value ?? min

  return (
    <div className={cn('flex flex-col gap-3', disabled && 'opacity-50')}>
      <div className="flex items-baseline justify-between gap-3">
        <Label className="font-sans text-[13px] font-medium text-slate-medium">{label}</Label>
        <span className="flex items-baseline gap-2">
          {sub && <span className="font-sans text-[11px] text-muted-foreground">{sub(v)}</span>}
          <span className="num text-[15px]">{v.toFixed(digits)}<span className="ml-0.5 font-sans text-[11px] text-muted-foreground">{unit}</span></span>
        </span>
      </div>
      <Slider
        min={min}
        max={max}
        step={step}
        value={[v]}
        disabled={disabled}
        onValueChange={([x]) => {
          dragging.current = true
          setLocal(x)
          send(command(x))
        }}
        onValueCommit={([x]) => {
          dragging.current = false
          send(command(x))
        }}
      />
    </div>
  )
}

/** Number field bound to a console parameter; sends on blur/Enter. */
export function ParamField({
  label, value, unit, command, className,
}: {
  label?: string
  value: number | undefined
  unit?: string
  command: (v: number) => string
  className?: string
}) {
  const [text, setText] = useState('')
  const [editing, setEditing] = useState(false)
  const shown = editing ? text : value === undefined ? '' : String(+value.toFixed(6))
  const commit = () => {
    setEditing(false)
    const v = Number(text)
    if (text.trim() !== '' && Number.isFinite(v) && v !== value) cmd(command(v))
  }
  return (
    <div className={cn('flex flex-col gap-1.5', className)}>
      {label && <Label className="eyebrow">{label}</Label>}
      <div className="relative">
        <Input
          className="num h-8 rounded-md border-stone bg-ivory/70 pr-10 text-[13px] shadow-none focus-visible:bg-ivory"
          inputMode="decimal"
          value={shown}
          onFocus={() => { setEditing(true); setText(shown) }}
          onChange={(e) => setText(e.target.value)}
          onBlur={commit}
          onKeyDown={(e) => { if (e.key === 'Enter') (e.target as HTMLInputElement).blur() }}
        />
        {unit && <span className="pointer-events-none absolute top-1/2 right-2.5 -translate-y-1/2 text-[11px] text-muted-foreground">{unit}</span>}
      </div>
    </div>
  )
}

export function Controls({ mode, setMode }: { mode: DriveMode; setMode: (m: DriveMode) => void }) {
  const p = useStore((s) => s.params)
  const tel = useStore((s) => s.tel)
  const status = useStore((s) => s.status)
  const pp = useStore((s) => s.polePairs)
  const [armed, setArmed] = useState(false)
  const [rampOpen, setRampOpen] = useState(false)

  const running = !!tel && tel.st !== 'idle'
  const isLab = status?.connected && status.app === 'test_08_drive_lab'
  const foc = mode === 'if' || mode === 'sl'
  const needRL = foc && (!p || p.R <= 0 || p.L <= 0)
  const canStart = isLab && armed && !running && !needRL

  // follow the board: while running show what really runs
  const runningMode: DriveMode | null = !tel || tel.st === 'idle' ? null
    : tel.st === 'foc' ? tel.kind : (tel.st === 'run' || tel.st === 'align') ? tel.mod : null
  useEffect(() => {
    if (runningMode && runningMode !== mode) setMode(runningMode)
    else if (!running && p && !foc && p.mode !== mode) setMode(p.mode)
  }, [p?.mode, runningMode]) // eslint-disable-line react-hooks/exhaustive-deps

  // disarm after every run: each start is a deliberate act
  const wasRunning = useRef(false)
  useEffect(() => {
    if (wasRunning.current && !running) setArmed(false)
    wasRunning.current = running
  }, [running])

  const start = () => {
    if (!canStart) return
    if (foc) cmd(mode)
    else {
      if (p?.mode !== mode) cmd(`mode ${mode}`)
      cmd('start')
    }
  }

  return (
    <div className="flex flex-col gap-7">
      <div>
        <div className="eyebrow mb-1">Inputs</div>
        <h2 className="font-voice text-[34px] leading-none tracking-tight">Drive</h2>
      </div>

      {/* mode */}
      <div className="flex flex-col gap-3">
        {(['Open loop', 'Closed current loop'] as const).map((g) => (
          <div key={g} className="flex flex-col gap-1">
            <span className="eyebrow">{g}</span>
            <div className="grid grid-cols-2">
              {MODES.filter((m) => m.group === g).map((m) => {
                const on = mode === m.id
                return (
                  <button
                    key={m.id}
                    disabled={running}
                    onClick={() => setMode(m.id)}
                    className={cn(
                      'group relative flex flex-col items-start gap-0.5 py-2.5 pr-3 pl-3.5 text-left transition-colors disabled:cursor-not-allowed',
                      !on && 'hover:bg-oat/40',
                    )}
                  >
                    <span className="absolute inset-y-1.5 left-0 w-px bg-stone" />
                    {on && (
                      <motion.span layoutId="mode-rule" className="absolute inset-y-1.5 left-0 w-[3px] bg-slate"
                        transition={{ type: 'spring', stiffness: 520, damping: 40 }} />
                    )}
                    <span className={cn('font-sans text-[14px]', on ? 'font-semibold text-slate' : 'text-slate-medium')}>{m.label}</span>
                    <span className="font-voice text-[12.5px] leading-tight text-muted-foreground">{m.hint}</span>
                  </button>
                )
              })}
            </div>
          </div>
        ))}
      </div>

      {/* live setpoints */}
      <div className="hair-top flex flex-col gap-6 pt-6">
        {!foc && (
          <LiveSlider label="Amplitude" unit="%" value={p?.amp} min={0} max={p?.ampmax ?? 60} step={0.5}
            command={(v) => `amp ${v.toFixed(1)}`}
            sub={(v) => p ? `≈ ${(v / 100 * p.vbus).toFixed(2)} V` : ''} />
        )}
        <LiveSlider
          label={mode === 'sl' ? 'Speed target' : 'Target frequency'}
          unit="Hz" value={p?.f} min={0} max={400} step={1} digits={0}
          command={(v) => `f ${Math.max(0.05, v).toFixed(1)}`}
          sub={(v) => `${rpmOf(v, pp).toFixed(0)} rpm`}
        />
        <LiveSlider label="Current limit" unit="A" value={p?.ilim} min={0.2} max={4.5} step={0.1}
          digits={2} command={(v) => `ilim ${v.toFixed(2)}`}
          sub={(v) => `trip ${Math.min(2 * v, 5.3).toFixed(1)} A`} />
        {foc && (
          <LiveSlider label="I/f current" unit="A" value={p?.ifa} min={0.1} max={4} step={0.1} digits={2}
            command={(v) => `set ifa ${v.toFixed(2)}`} />
        )}
        {mode === 'sl' && (
          <LiveSlider label="Handover speed" unit="Hz" value={p?.fsw} min={5} max={150} step={1} digits={0}
            command={(v) => `set fsw ${v.toFixed(0)}`} />
        )}
        {foc && (
          <div className="flex items-center justify-between">
            <Label className="font-sans text-[13px] font-medium text-slate-medium">FOC modulation</Label>
            <Select value={p?.focmod} onValueChange={(v) => cmd(`set focmod ${v}`)}>
              <SelectTrigger className="h-8 w-28 rounded-md border-stone bg-ivory/70 text-xs shadow-none"><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value="sine">Sine</SelectItem>
                <SelectItem value="svpwm">SVPWM</SelectItem>
                <SelectItem value="dpwm">DPWM</SelectItem>
              </SelectContent>
            </Select>
          </div>
        )}
      </div>

      <Collapsible open={rampOpen} onOpenChange={setRampOpen} className="hair-top pt-3">
        <CollapsibleTrigger className="flex w-full items-center justify-between py-1 text-left">
          <span className="eyebrow">Start-up, ramp & supply</span>
          <ChevronDown className={cn('size-4 text-muted-foreground transition-transform', rampOpen && 'rotate-180')} />
        </CollapsibleTrigger>
        <CollapsibleContent className="data-[state=open]:animate-in data-[state=open]:fade-in-0">
          <div className="grid grid-cols-2 gap-x-3 gap-y-4 pt-4">
            <ParamField label="Supply" unit="V" value={p?.vbus} command={(v) => `vbus ${v}`} />
            <ParamField label="Start freq." unit="Hz" value={p?.fstart} command={(v) => `fstart ${v}`} />
            <ParamField label="Accel." unit="Hz/s" value={p?.acc0} command={(v) => `accel ${v} ${p?.acc1 ?? 0.1}`} />
            <ParamField label="Accel. per Hz" unit="1/s" value={p?.acc1} command={(v) => `accel ${p?.acc0 ?? 5} ${v}`} />
            <ParamField label="Align time" unit="ms" value={p?.align} command={(v) => `align ${Math.round(v)}`} />
            <ParamField label="Amp. ceiling" unit="%" value={p?.ampmax} command={(v) => `set ampmax ${v}`} />
          </div>
          <p className="mt-3 font-voice text-[13px] leading-snug text-muted-foreground">
            The board has no supply sense: set the real supply voltage here.
          </p>
        </CollapsibleContent>
      </Collapsible>

      <AnimatePresence>
        {needRL && (
          <motion.div
            initial={{ opacity: 0, height: 0 }} animate={{ opacity: 1, height: 'auto' }} exit={{ opacity: 0, height: 0 }}
            className="overflow-hidden"
          >
            <div className="flex flex-col gap-3 border-l-2 border-warn bg-manilla/50 py-3 pr-3 pl-4">
              <p className="font-voice text-[14.5px] leading-snug">
                FOC needs the motor's R and L. Measure them, or load the bring-up values of the drone motor.
              </p>
              <div className="flex gap-2">
                <Button size="sm" variant="outline" className="rounded-md border-slate bg-transparent" disabled={!isLab || running} onClick={() => cmd('rl')}>Measure R/L</Button>
                <Button size="sm" variant="ghost" className="gap-1.5 rounded-md" disabled={!isLab}
                  onClick={() => { cmd('set R 0.1378'); cmd('set L 11.3'); cmd('set lam 0.0009') }}>
                  <Wand2 className="size-3.5" />Drone motor preset
                </Button>
              </div>
            </div>
          </motion.div>
        )}
      </AnimatePresence>

      {/* run */}
      <div className="rule-top flex flex-col gap-4 pt-5">
        <label className="flex items-center justify-between gap-3">
          <span className="flex flex-col">
            <span className="font-sans text-[13px] font-semibold">Arm the bridge</span>
            <span className="font-voice text-[12.5px] text-muted-foreground">Required before every start</span>
          </span>
          <Switch checked={armed} onCheckedChange={setArmed} disabled={!isLab || running} />
        </label>
        {running ? (
          <motion.div key="stop" initial={{ opacity: 0.3, y: 4 }} animate={{ opacity: 1, y: 0 }}>
            <Button size="xl" className="w-full rounded-md bg-slate text-ivory hover:bg-slate-medium" onClick={estop}>
              <Square className="size-4 fill-current" />Stop the motor
            </Button>
          </motion.div>
        ) : (
          <motion.div key="start" initial={{ opacity: 0.3, y: 4 }} animate={{ opacity: 1, y: 0 }}
            whileTap={canStart ? { scale: 0.985 } : undefined}>
            <Button size="xl" variant="clay" className="w-full rounded-md" disabled={!canStart} onClick={start}>
              <Play className="size-4 fill-current" />
              Start {MODES.find((m) => m.id === mode)?.label}
            </Button>
          </motion.div>
        )}
        <p className="font-sans text-xs text-muted-foreground">
          {!status?.connected ? 'No board connected.'
            : !isLab ? `Board runs ${status.app}: flash the drive lab from Tests.`
            : !armed && !running ? 'Arm the bridge to enable Start.'
            : running ? 'Esc or Space stops at any time.' : 'Ready.'}
        </p>
      </div>
    </div>
  )
}
