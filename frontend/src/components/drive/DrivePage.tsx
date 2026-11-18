import { AnimatePresence } from 'motion/react'
import { useState } from 'react'
import { AlertTriangle, CheckCircle2, Info, OctagonX } from 'lucide-react'
import { AnimatedListItem } from '@/components/ui/animated-list'
import { NumberTicker } from '@/components/ui/number-ticker'
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs'
import { rpmOf, useStore } from '@/lib/store'
import { cn } from '@/lib/utils'
import { Meter, PHASES, Section, Stat, StatRow, StateBadge, fmt } from '../common'
import { Controls, type DriveMode } from './Controls'
import { LiveChart } from './LiveChart'
import { PhaseBars } from './PhaseBars'
import { RotorDial } from './RotorDial'

const MODE_LABEL: Record<string, string> = { '6step': '6-step', sine: 'Sine', svpwm: 'SVPWM', dpwm: 'DPWM' }

export function DrivePage() {
  const [mode, setMode] = useState<DriveMode>('6step')
  return (
    <div className="grid gap-x-12 gap-y-10 xl:grid-cols-[320px_minmax(0,1fr)] 2xl:grid-cols-[360px_minmax(0,1fr)]">
      <aside className="xl:border-r xl:border-stone xl:pr-10">
        <div className="xl:sticky xl:top-20">
          <Controls mode={mode} setMode={setMode} />
        </div>
      </aside>
      <div className="flex min-w-0 flex-col gap-12">
        <Hero />
        <div className="grid gap-x-12 gap-y-12 2xl:grid-cols-[minmax(0,1.35fr)_minmax(0,1fr)]">
          <Trends />
          <Measured />
        </div>
        <div className="grid gap-x-12 gap-y-12 lg:grid-cols-2 2xl:grid-cols-[minmax(0,1.35fr)_minmax(0,1fr)]">
          <Outputs />
          <Events />
        </div>
      </div>
    </div>
  )
}

// ---- headline: speed, current, angle --------------------------------------------
function Hero() {
  const tel = useStore((s) => s.tel)
  const p = useStore((s) => s.params)
  const hello = useStore((s) => s.hello)
  const pp = useStore((s) => s.polePairs)
  const setPP = useStore((s) => s.setPolePairs)
  const live = !!tel && tel.st !== 'idle'
  const foc = tel?.st === 'foc'
  const fEl = !tel ? 0 : foc ? (tel.obs ? tel.fpll : tel.fol) : tel.f
  const rpm = rpmOf(fEl, pp)
  const ilim = p?.ilim ?? 2
  const trip = Math.min(2 * ilim, hello?.itrip ?? 5.3)
  const ifilt = live ? tel!.ifilt : 0
  const label = foc ? (tel!.kind === 'sl' ? (tel!.obs ? 'Sensorless · locked' : 'Sensorless · I/f start') : 'I/f') : undefined

  return (
    <Section eyebrow="Measured" delay={0.02}
      action={
        <div className="flex items-center gap-5">
          {live && !foc && <span className="font-sans text-[12px] text-muted-foreground">{MODE_LABEL[tel!.mod]}</span>}
          {tel?.st === 'idle' && tel.stop && <span className="font-sans text-[12px] text-muted-foreground">last stop: {tel.stop}</span>}
          <StateBadge st={tel?.st} label={label} />
        </div>
      }>
      <div className="grid items-center gap-x-12 gap-y-8 lg:grid-cols-[minmax(0,1.2fr)_minmax(0,1fr)_auto]">
        {/* speed */}
        <div className="flex flex-col gap-6">
          <div>
            <div className="eyebrow mb-1">Mechanical speed · {foc && tel?.obs ? 'observer' : 'commanded'}</div>
            <div className="flex items-baseline gap-3">
              <NumberTicker value={Math.round(rpm)} className="font-voice text-[96px] leading-[0.9] tracking-[-0.02em] text-slate 2xl:text-[120px]" />
              <span className="font-voice text-2xl text-muted-foreground">rpm</span>
            </div>
          </div>
          <StatRow className="grid-cols-2 sm:grid-cols-4">
            <Stat label="Electrical" value={fmt(fEl, 1)} unit="Hz" />
            <Stat label="Target" value={fmt(p?.f, 0)} unit="Hz" tone="muted" />
            <Stat label="Angle θ" value={fmt(tel?.th, 0)} unit="°" />
            <div className="flex flex-col gap-1.5">
              <span className="eyebrow">Pole pairs</span>
              <div className="flex items-baseline gap-2.5">
                {[1, 4, 7, 14].map((n) => (
                  <button key={n} onClick={() => setPP(n)}
                    className={cn('num border-b text-[15px] leading-none transition-colors',
                      pp === n ? 'border-slate text-slate' : 'border-transparent text-cloud-dark hover:text-slate')}>{n}</button>
                ))}
              </div>
            </div>
          </StatRow>
        </div>

        {/* current */}
        <div className="flex flex-col gap-5 lg:border-l lg:border-stone lg:pl-12">
          <div>
            <div className="eyebrow mb-1">Phase current |I|</div>
            <div className="flex items-baseline gap-2">
              <span className={cn('num text-5xl leading-none', ifilt > ilim && 'text-clay-deep')}>{ifilt.toFixed(2)}</span>
              <span className="font-sans text-sm text-muted-foreground">A</span>
            </div>
          </div>
          <Meter value={ifilt} max={Math.max(trip * 1.1, 1)} color={ifilt > ilim ? 'var(--color-clay-deep)' : 'var(--color-slate)'}
            marks={[{ at: ilim, label: `limit ${ilim.toFixed(1)}` }, { at: trip, label: `trip ${trip.toFixed(1)}`, strong: true }]} />
          <StatRow className="grid-cols-3">
            <Stat size="sm" label="Peak" value={fmt(live ? Math.max(...tel!.pk) : 0)} unit="A"
              hint="Largest |phase current| sample in the last telemetry window" />
            <Stat size="sm" label="Conduction" value={live ? `${tel!.flow}` : '—'} unit="%"
              tone={live && tel!.flow < 60 ? 'warn' : undefined}
              hint="Samples with |I| > 0.14 A. Low values mean the bridge is not passing current." />
            <Stat size="sm" label="nFAULT" value={tel ? (tel.nf ? 'LOW' : 'high') : '—'} tone={tel?.nf ? 'bad' : 'good'}
              hint="DRV8316 fault line. High = OK." />
          </StatRow>
        </div>

        {/* angle */}
        <div className="flex flex-col items-center gap-2 justify-self-center">
          <RotorDial tel={tel} ilim={ilim} size={250} />
          <div className="flex gap-5 font-sans text-[11px] text-muted-foreground">
            <span className="inline-flex items-center gap-1.5"><span className="h-px w-4 bg-slate" />angle θ</span>
            <span className="inline-flex items-center gap-1.5"><span className="h-1 w-4 rounded-[1px] bg-cloud-dark/70" />current vector</span>
            {tel?.mod === '6step' && live && <span className="inline-flex items-center gap-1.5"><span className="size-2.5 bg-manilla" />sector</span>}
          </div>
        </div>
      </div>
    </Section>
  )
}

// ---- measured: per phase + feedback -------------------------------------------------
function Measured() {
  const tel = useStore((s) => s.tel)
  const csa = useStore((s) => s.csa)
  const hello = useStore((s) => s.hello)
  const live = !!tel && tel.st !== 'idle'
  const foc = tel?.st === 'foc'
  const off = csa?.off ?? tel?.off

  return (
    <Section eyebrow="Measured by the board" title="Currents & sensing" delay={0.08}>
      <table className="w-full text-left">
        <thead>
          <tr className="border-b border-stone">
            {['Phase', 'Now', 'RMS', 'Peak', 'SOx offset'].map((h, i) => (
              <th key={h} className={cn('eyebrow pb-2 font-semibold', i > 0 && 'text-right')}>{h}</th>
            ))}
          </tr>
        </thead>
        <tbody>
          {PHASES.map((ph, k) => {
            const v = off?.[k]
            const okOff = v !== undefined && v >= 1.3 && v <= 2.0
            return (
              <tr key={ph.name} className="border-b border-stone/60">
                <td className="py-3">
                  <span className="inline-flex items-center gap-2.5 font-sans text-[13px] font-semibold">
                    <span className="h-3 w-[3px]" style={{ background: ph.color }} />{ph.name}
                    {tel?.bad === k && <span className="font-normal text-warn">rebuilt</span>}
                  </span>
                </td>
                <td className="num py-3 text-right text-[15px]">{fmt(live ? tel!.i[k] : tel?.iidle[k])}<Unit u="A" /></td>
                <td className="num py-3 text-right text-[15px]">{fmt(live ? tel!.rms[k] : 0)}<Unit u="A" /></td>
                <td className="num py-3 text-right text-[15px]">{fmt(live ? tel!.pk[k] : 0)}<Unit u="A" /></td>
                <td className={cn('num py-3 text-right text-[15px]', v !== undefined && !okOff && 'text-clay-deep')}>{fmt(v, 3)}<Unit u="V" /></td>
              </tr>
            )
          })}
        </tbody>
      </table>

      <StatRow className="grid-cols-2 sm:grid-cols-4">
        <Stat size="sm" label="KCL residual" value={tel && tel.res >= 0 ? fmt(tel.res, 3) : '—'} unit="A"
          hint="RMS of iA + iB + iC on samples where all three SOx are valid. Should be near 0." />
        <Stat size="sm" label="Run max |I|" value={fmt(tel?.imax)} unit="A" hint="Largest sample since the run started" />
        <Stat size="sm" label="Max frequency" value={fmt(tel?.fmax, 1)} unit="Hz" />
        <Stat size="sm" label="CSA gain" value={fmt(hello?.csa ?? 0.15, 2)} unit="V/A" tone="muted"
          hint="Offsets must sit in 1.3–2.0 V (VREF/2 ≈ 1.65 V); a phase outside it is rebuilt from the other two." />
      </StatRow>

      {foc && (
        <div className="hair-top flex flex-col gap-5 pt-5">
          <div className="eyebrow">Field-oriented control</div>
          <StatRow className="grid-cols-2 sm:grid-cols-4">
            <Stat size="sm" label="id / ref" value={`${fmt(tel!.id)} / ${fmt(tel!.idr)}`} unit="A" />
            <Stat size="sm" label="iq / ref" value={`${fmt(tel!.iq)} / ${fmt(tel!.iqr)}`} unit="A" />
            <Stat size="sm" label="λ est. / use" value={`${fmt(tel!.lam * 1000, 3)} / ${fmt(tel!.lamu * 1000, 3)}`} unit="mWb"
              hint="Flux linkage: back-EMF estimate in the I/f frame, and the value the observer uses" />
            <Stat size="sm" label="Dropouts" value={`${tel!.drop}`} unit="%" tone={tel!.drop > 10 ? 'warn' : undefined}
              hint="Samples where |I| fell below 30 % of the reference" />
          </StatRow>
          <StatRow className="grid-cols-2 sm:grid-cols-4">
            <Stat size="sm" label="PLL speed" value={fmt(tel!.fpll, 1)} unit="Hz" />
            <Stat size="sm" label="Open-loop speed" value={fmt(tel!.fol, 1)} unit="Hz" />
            <Stat size="sm" label="Observer" value={tel!.obs ? 'locked' : 'off'} tone={tel!.obs ? 'good' : 'muted'} />
            <Stat size="sm" label="θ observer" value={fmt(tel!.tho, 0)} unit="°" />
          </StatRow>
        </div>
      )}
    </Section>
  )
}

const Unit = ({ u }: { u: string }) => <span className="ml-1 font-sans text-[11px] text-muted-foreground">{u}</span>

// ---- measured: history ------------------------------------------------------------
function Trends() {
  const hist = useStore((s) => s.hist)
  const p = useStore((s) => s.params)
  const pp = useStore((s) => s.polePairs)
  const data = hist.slice(-400)
  const anyFoc = data.some((d) => d.st === 'foc')
  const span = data.length ? data[data.length - 1].ts - data[0].ts : 0

  return (
    <Tabs defaultValue="speed" className="min-w-0">
      <Section eyebrow={`Measured · last ${span.toFixed(0)} s`} title="Trends" delay={0.05}
        action={
          <TabsList variant="line" className="h-8 gap-4">
            <TabsTrigger value="speed" className="px-0">Speed</TabsTrigger>
            <TabsTrigger value="current" className="px-0">Phase current</TabsTrigger>
            <TabsTrigger value="dq" className="px-0" disabled={!anyFoc}>dq currents</TabsTrigger>
            <TabsTrigger value="drive" className="px-0">Amplitude</TabsTrigger>
          </TabsList>
        }>
        <TabsContent value="speed">
          <LiveChart data={data} unit="rpm" digits={0} height={300} series={[
            { key: 'rpm', label: 'Speed', color: 'var(--color-slate)', get: (d) => rpmOf(d.st === 'foc' ? (d.obs ? d.fpll : d.fol) : d.f, pp) },
            { key: 'tgt', label: 'Target', color: 'var(--color-cloud-dark)', dashed: true, get: () => rpmOf(p?.f ?? 0, pp) },
          ]} />
        </TabsContent>
        <TabsContent value="current">
          <LiveChart data={data} unit="A rms" height={300} series={PHASES.map((ph, k) => ({
            key: `rms${k}`, label: `Phase ${ph.name}`, color: ph.color, get: (d) => d.rms[k],
          }))} />
        </TabsContent>
        <TabsContent value="dq">
          <LiveChart data={data} unit="A" zero height={300} series={[
            { key: 'id', label: 'id', color: 'var(--color-phase-a)' },
            { key: 'idr', label: 'id ref', color: 'var(--color-phase-a)', dashed: true },
            { key: 'iq', label: 'iq', color: 'var(--color-phase-b)' },
            { key: 'iqr', label: 'iq ref', color: 'var(--color-phase-b)', dashed: true },
          ]} />
        </TabsContent>
        <TabsContent value="drive">
          <LiveChart data={data} unit="%" height={300} series={[
            { key: 'amp', label: 'Applied', color: 'var(--color-slate)' },
            { key: 'cap', label: 'Limiter ceiling', color: 'var(--color-cloud-dark)', dashed: true },
          ]} />
        </TabsContent>
      </Section>
    </Tabs>
  )
}

// ---- outputs ------------------------------------------------------------------
function Outputs() {
  const tel = useStore((s) => s.tel)
  const p = useStore((s) => s.params)
  const live = !!tel && tel.st !== 'idle'
  const foc = tel?.st === 'foc'
  const vmag = foc ? Math.hypot(tel!.vd, tel!.vq) : live && p ? (tel!.amp / 100) * p.vbus : 0
  return (
    <Section eyebrow="Outputs" title="Bridge" delay={0.11}
      action={<span className="font-sans text-[12px] text-muted-foreground">TIM1 · 20 kHz · 1 µs dead time</span>}>
      <div className="grid gap-x-12 gap-y-8 md:grid-cols-[minmax(0,1.3fr)_minmax(0,1fr)]">
        <PhaseBars tel={tel} />
        <div className="grid content-start grid-cols-2 gap-x-6 gap-y-5">
          <Stat size="sm" label="Voltage |v|" value={fmt(vmag)} unit="V"
            hint={foc ? '|vd, vq| from the current controllers' : 'Amplitude × supply'} />
          <Stat size="sm" label="Commutation step" value={live && tel!.step >= 0 ? `${tel!.step + 1} / 6` : '—'} />
          <Stat size="sm" label="Amplitude" value={!foc ? fmt(tel?.amp, 1) : '—'} unit="%" hint="After the current limiter" />
          <Stat size="sm" label="Limiter ceiling" value={!foc ? fmt(tel?.cap, 1) : '—'} unit="%"
            hint="The phase-current limiter lowers this when |I| exceeds the limit" />
          <Stat size="sm" label="vd" value={foc ? fmt(tel!.vd, 3) : '—'} unit="V" />
          <Stat size="sm" label="vq" value={foc ? fmt(tel!.vq, 3) : '—'} unit="V" />
        </div>
      </div>
    </Section>
  )
}

// ---- events ---------------------------------------------------------------------
function Events() {
  const events = useStore((s) => s.events)
  return (
    <Section eyebrow="Events" title="Run log" delay={0.14}>
      {events.length === 0 ? (
        <p className="font-voice text-[15px] text-muted-foreground">Stops, trips and measurements appear here.</p>
      ) : (
        <div className="flex flex-col">
          <AnimatePresence initial={false}>
            {events.slice(0, 6).map((e) => {
              const Icon = { info: Info, ok: CheckCircle2, warn: AlertTriangle, error: OctagonX }[e.level]
              return (
                <AnimatedListItem key={e.id}>
                  <div className="flex items-start gap-3 border-b border-stone/60 py-2.5">
                    <Icon className={cn('mt-0.5 size-4 shrink-0', e.level === 'error' || e.level === 'warn' ? 'text-clay-deep' : e.level === 'ok' ? 'text-good' : 'text-muted-foreground')} />
                    <div className="min-w-0 flex-1 text-[13.5px] leading-snug">{e.text}</div>
                    <div className="num shrink-0 text-[11px] text-muted-foreground">{new Date(e.at).toLocaleTimeString()}</div>
                  </div>
                </AnimatedListItem>
              )
            })}
          </AnimatePresence>
        </div>
      )}
    </Section>
  )
}
