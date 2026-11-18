import { Camera } from 'lucide-react'
import { useEffect, useMemo, useState } from 'react'
import { Button } from '@/components/ui/button'
import { Label } from '@/components/ui/label'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select'
import { Switch } from '@/components/ui/switch'
import { cmd } from '@/lib/api'
import { useStore } from '@/lib/store'
import { PHASES, Section, SectionTitle, Stat, StatRow, fmt } from './common'
import { LiveChart } from './drive/LiveChart'

const sel = 'h-8 rounded-md border-stone bg-ivory/70 text-xs shadow-none'

/** Snapshots of the 20 kHz control-loop trace ("snap" command). */
export function ScopePage() {
  const wave = useStore((s) => s.wave)
  const p = useStore((s) => s.params)
  const tel = useStore((s) => s.tel)
  const isLab = useStore((s) => s.status?.app === 'test_08_drive_lab' && s.status.connected)
  const [n, setN] = useState('256')
  const [auto, setAuto] = useState(false)
  const [period, setPeriod] = useState('500')

  useEffect(() => {
    if (!auto || !isLab) return
    const t = setInterval(() => cmd(`snap ${n}`), Number(period))
    return () => clearInterval(t)
  }, [auto, isLab, n, period])

  const rows = useMemo(() => {
    if (!wave) return []
    const dt = wave.dt_us / 1000
    return wave.ia.map((_, k) => ({
      t: k * dt, ia: wave.ia[k], ib: wave.ib[k], ic: wave.ic[k],
      da: wave.da[k], db: wave.db[k], dc: wave.dc[k], th: wave.th[k],
    }))
  }, [wave])

  const stats = useMemo(() => {
    if (!wave || !wave.n) return null
    const rms = (a: number[]) => Math.sqrt(a.reduce((s, x) => s + x * x, 0) / a.length)
    const pk = (a: number[]) => Math.max(...a.map(Math.abs))
    return { rms: [rms(wave.ia), rms(wave.ib), rms(wave.ic)], pk: [pk(wave.ia), pk(wave.ib), pk(wave.ic)] }
  }, [wave])

  return (
    <div className="flex flex-col gap-10">
      <SectionTitle eyebrow="Measured · 20 kHz control loop" title="Scope">
        <div className="flex flex-wrap items-center gap-x-6 gap-y-3">
          <div className="flex items-center gap-2">
            <Label className="eyebrow">Samples</Label>
            <Select value={n} onValueChange={setN}>
              <SelectTrigger className={`${sel} w-20`}><SelectValue /></SelectTrigger>
              <SelectContent>{['128', '256', '512'].map((v) => <SelectItem key={v} value={v}>{v}</SelectItem>)}</SelectContent>
            </Select>
          </div>
          <div className="flex items-center gap-2">
            <Label className="eyebrow">Spacing</Label>
            <Select value={String(p?.trdec ?? 1)} onValueChange={(v) => cmd(`set trdec ${v}`)} disabled={!isLab}>
              <SelectTrigger className={`${sel} w-24`}><SelectValue /></SelectTrigger>
              <SelectContent>
                {[1, 2, 5, 10, 20].map((v) => <SelectItem key={v} value={String(v)}>{v * 50} µs</SelectItem>)}
              </SelectContent>
            </Select>
          </div>
          <div className="flex items-center gap-2 border-l border-stone pl-6">
            <Label className="eyebrow">Auto</Label>
            <Switch checked={auto} onCheckedChange={setAuto} disabled={!isLab} />
            <Select value={period} onValueChange={setPeriod}>
              <SelectTrigger className={`${sel} w-22`}><SelectValue /></SelectTrigger>
              <SelectContent>{['250', '500', '1000', '2000'].map((v) => <SelectItem key={v} value={v}>{v} ms</SelectItem>)}</SelectContent>
            </Select>
          </div>
          <Button className="h-8 gap-2 rounded-md bg-slate px-4 text-ivory hover:bg-slate-medium" disabled={!isLab} onClick={() => cmd(`snap ${n}`)}>
            <Camera className="size-3.5" />Capture
          </Button>
        </div>
      </SectionTitle>

      <Section
        eyebrow={wave ? <>{wave.n} samples · <span className="normal-case">{wave.dt_us} µs</span> apart · {wave.frozen ? 'trace frozen at the last stop' : `live (${wave.st})`}</> : 'No capture yet'}
        title="Phase currents"
        action={stats && (
          <StatRow className="grid-cols-3 gap-x-0">
            {PHASES.map((ph, k) => (
              <Stat key={ph.name} size="sm" label={<span style={{ color: ph.color }}>{ph.name} · rms / peak</span>}
                value={`${fmt(stats.rms[k])} / ${fmt(stats.pk[k])}`} unit="A" />
            ))}
          </StatRow>
        )}>
        {rows.length ? (
          <LiveChart data={rows} x="t" xUnit="ms" unit="A" zero height={340} series={PHASES.map((ph, k) => ({
            key: ['ia', 'ib', 'ic'][k], label: `i${ph.name} (raw SOx)`, color: ph.color,
          }))} />
        ) : (
          <Empty running={!!tel && tel.st !== 'idle'} />
        )}
      </Section>

      <div className="grid gap-x-12 gap-y-10 lg:grid-cols-2">
        <Section eyebrow="Outputs" title="Duty cycles" delay={0.05}>
          {rows.length ? (
            <LiveChart data={rows} x="t" xUnit="ms" unit="%" domain={[0, 'auto']} height={240} series={PHASES.map((ph, k) => ({
              key: ['da', 'db', 'dc'][k], label: `d${ph.name}`, color: ph.color,
            }))} />
          ) : <Empty />}
        </Section>
        <Section eyebrow="Commanded" title="Electrical angle" delay={0.08}>
          {rows.length ? (
            <LiveChart data={rows} x="t" xUnit="ms" unit="°" domain={[0, 360]} digits={0} height={240}
              series={[{ key: 'th', label: 'θ', color: 'var(--color-slate)' }]} />
          ) : <Empty />}
        </Section>
      </div>
    </div>
  )
}

function Empty({ running }: { running?: boolean }) {
  return (
    <div className="flex h-56 flex-col items-center justify-center gap-1 border border-dashed border-stone">
      <p className="font-voice text-lg">Nothing captured yet.</p>
      <p className="font-voice text-sm text-muted-foreground">
        {running ? 'Press Capture, or switch on Auto.' : 'Start the motor, then capture. After a stop the trace holds the samples before it.'}
      </p>
    </div>
  )
}
