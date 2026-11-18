import { AnimatePresence, motion } from 'motion/react'
import { CheckCircle2, CircleDashed, FlaskConical, Loader2, OctagonX, Play, RotateCcw, X } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import { toast } from 'sonner'
import {
  AlertDialog, AlertDialogAction, AlertDialogCancel, AlertDialogContent, AlertDialogDescription,
  AlertDialogFooter, AlertDialogHeader, AlertDialogTitle,
} from '@/components/ui/alert-dialog'
import { BlurFade } from '@/components/ui/blur-fade'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardDescription, CardFooter, CardHeader, CardTitle } from '@/components/ui/card'
import { Input } from '@/components/ui/input'
import { Label } from '@/components/ui/label'
import { Progress } from '@/components/ui/progress'
import { ScrollArea } from '@/components/ui/scroll-area'
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from '@/components/ui/table'
import { api } from '@/lib/api'
import { useStore } from '@/lib/store'
import type { Job, TestDef } from '@/lib/types'
import { cn } from '@/lib/utils'
import { SafetyBadge, SectionTitle, fmt } from './common'

export function TestsPage() {
  const [tests, setTests] = useState<TestDef[]>([])
  const [confirm, setConfirm] = useState<{ t: TestDef; values: Record<string, number> } | null>(null)
  const job = useStore((s) => s.job)
  const status = useStore((s) => s.status)
  const busy = job?.status === 'running'

  useEffect(() => { api.get<TestDef[]>('/api/tests').then(setTests).catch(() => {}) }, [])

  const run = async (t: TestDef, values: Record<string, number>) => {
    try {
      await api.post(`/api/tests/${t.id}/run`, { values })
    } catch (e) {
      toast.error((e as Error).message)
    }
  }
  const ask = (t: TestDef, values: Record<string, number>) =>
    t.safety === 'safe' ? run(t, values) : setConfirm({ t, values })

  const groups = ['Drive lab', 'Board']
  const onLab = status?.app === 'test_08_drive_lab'

  return (
    <div className="flex flex-col gap-6">
      <BlurFade inView={false}>
        <SectionTitle eyebrow="Verification" title="Tests">
          {!onLab && status?.connected && (
            <Button variant="ivory" className="gap-2" disabled={busy}
              onClick={() => api.post('/api/flash', { app: 'test_08_drive_lab' }).catch((e) => toast.error(e.message))}>
              <RotateCcw className="size-4" />Flash the drive lab back
            </Button>
          )}
        </SectionTitle>
        <p className="mt-2 max-w-3xl font-voice text-[17px] leading-relaxed text-muted-foreground">
          Drive-lab procedures run on the firmware that is already on the board. Board tests build and flash their
          own application over USB, capture its console and read the results from it; flash the drive lab back
          afterwards to drive the motor again.
        </p>
      </BlurFade>

      <AnimatePresence>{job && <JobPanel key={job.id} job={job} />}</AnimatePresence>

      {groups.map((g, gi) => (
        <section key={g} className="flex flex-col gap-4">
          <div className="flex items-baseline gap-3">
            <h3 className="font-voice text-xl">{g === 'Board' ? 'Board applications' : 'Drive-lab procedures'}</h3>
            <span className="font-sans text-xs text-muted-foreground">
              {g === 'Board' ? 'flash + capture' : onLab ? 'drive lab running' : 'needs the drive lab'}
            </span>
          </div>
          <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-3">
            {tests.filter((t) => t.group === g).map((t, i) => (
              <BlurFade key={t.id} delay={0.03 * i + gi * 0.05} inView={false}>
                <TestCard t={t} disabled={busy || !status?.connected || (t.kind === 'lab' && !onLab)}
                  running={busy && job?.test === t.id} onRun={(v) => ask(t, v)} />
              </BlurFade>
            ))}
          </div>
        </section>
      ))}

      <AlertDialog open={!!confirm} onOpenChange={(o) => !o && setConfirm(null)}>
        <AlertDialogContent className="rounded-3xl bg-ivory">
          <AlertDialogHeader>
            <AlertDialogTitle className="font-voice text-2xl font-normal">
              {confirm?.t.safety === 'motor' ? 'The motor will turn' : 'The rotor will move'}
            </AlertDialogTitle>
            <AlertDialogDescription className="font-voice text-base">
              {confirm?.t.title}: {confirm?.t.covers}. Keep the shaft free and the supply current-limited. STOP,
              Esc or Space ends it at any time.
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>Cancel</AlertDialogCancel>
            <AlertDialogAction className="bg-clay text-white hover:bg-clay-deep"
              onClick={() => { if (confirm) run(confirm.t, confirm.values); setConfirm(null) }}>
              Run test
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
    </div>
  )
}

function TestCard({ t, disabled, running, onRun }: {
  t: TestDef; disabled: boolean; running: boolean; onRun: (v: Record<string, number>) => void
}) {
  const [vals, setVals] = useState<Record<string, number>>(() =>
    Object.fromEntries((t.inputs ?? []).map((i) => [i.key, i.default])))
  return (
    <Card className={cn('h-full gap-4 transition-colors', running && 'bg-manilla')}>
      <CardHeader>
        <div className="flex items-center justify-between gap-2">
          <span className="eyebrow">{t.kind === 'app' ? t.app : `${t.steps} step${t.steps === 1 ? '' : 's'}`}</span>
          <SafetyBadge level={t.safety} />
        </div>
        <CardTitle className="font-voice text-xl font-normal">{t.title}</CardTitle>
        <CardDescription className="font-voice text-[15px] leading-snug">{t.covers}</CardDescription>
      </CardHeader>
      {!!t.inputs?.length && (
        <CardContent className="grid grid-cols-2 gap-3">
          {t.inputs.map((i) => (
            <div key={i.key} className="flex flex-col gap-1">
              <Label className="eyebrow">{i.label}</Label>
              <div className="relative">
                <Input type="number" min={i.min} max={i.max} step={i.step} value={vals[i.key]}
                  onChange={(e) => setVals({ ...vals, [i.key]: Number(e.target.value) })}
                  className="num h-8 rounded-xl border-stone bg-ivory pr-9 text-xs" />
                <span className="pointer-events-none absolute top-1/2 right-2.5 -translate-y-1/2 text-[10px] text-muted-foreground">{i.unit}</span>
              </div>
            </div>
          ))}
        </CardContent>
      )}
      <CardFooter className="mt-auto border-none bg-transparent">
        <Button variant="ivory" className="ml-auto gap-2 px-5" disabled={disabled} onClick={() => onRun(vals)}>
          {running ? <Loader2 className="size-4 animate-spin" /> : <Play className="size-3.5" />}
          {running ? 'Running' : 'Run'}
        </Button>
      </CardFooter>
    </Card>
  )
}

const STATUS_UI: Record<Job['status'], { label: string; icon: typeof CheckCircle2; cls: string }> = {
  running: { label: 'Running', icon: Loader2, cls: 'bg-slate text-ivory' },
  passed: { label: 'Passed', icon: CheckCircle2, cls: 'bg-good text-ivory' },
  done: { label: 'Done', icon: CircleDashed, cls: 'bg-oat text-slate' },
  failed: { label: 'Failed', icon: OctagonX, cls: 'bg-clay-deep text-white' },
  error: { label: 'Error', icon: OctagonX, cls: 'bg-clay-deep text-white' },
  cancelled: { label: 'Cancelled', icon: X, cls: 'bg-oat text-slate' },
}

function JobPanel({ job }: { job: Job }) {
  const ui = STATUS_UI[job.status]
  const logEnd = useRef<HTMLDivElement>(null)
  useEffect(() => { logEnd.current?.scrollIntoView({ block: 'nearest' }) }, [job.log.length])
  const pct = job.steps ? Math.min(100, ((job.status === 'running' ? job.step - 1 : job.step) / job.steps) * 100) : 0
  const raws = job.results.filter((r) => r.kind === 'raw')
  const others = job.results.filter((r) => r.kind !== 'raw')
  const Icon = ui.icon

  return (
    <motion.div initial={{ opacity: 0, y: 12 }} animate={{ opacity: 1, y: 0 }} exit={{ opacity: 0, y: -8 }}>
      <Card className="gap-4 bg-ivory">
        <CardHeader className="flex flex-row flex-wrap items-start justify-between gap-3">
          <div>
            <div className="eyebrow flex items-center gap-2"><FlaskConical className="size-3.5" />Current test</div>
            <CardTitle className="font-voice text-2xl font-normal">{job.title}</CardTitle>
            <CardDescription className="font-sans text-xs">{job.phase}{job.verdict ? ` · ${job.verdict}` : ''}</CardDescription>
          </div>
          <div className="flex items-center gap-2">
            <motion.span layout className={cn('inline-flex h-7 items-center gap-1.5 rounded-full px-3 text-xs font-semibold', ui.cls)}>
              <Icon className={cn('size-3.5', job.status === 'running' && 'animate-spin')} />{ui.label}
            </motion.span>
            {job.status === 'running' && (
              <Button variant="outline" size="sm" className="rounded-xl" onClick={() => api.post('/api/jobs/cancel')}>Cancel</Button>
            )}
          </div>
        </CardHeader>
        <CardContent className="flex flex-col gap-4">
          {job.steps > 0 && <Progress value={pct} className="h-1.5 bg-oat [&>div]:bg-slate" />}

          {job.metrics.length > 0 && (
            <div className="grid grid-cols-2 gap-3 sm:grid-cols-3 lg:grid-cols-6">
              {job.metrics.map((m) => (
                <motion.div key={m.label} initial={{ opacity: 0, scale: 0.96 }} animate={{ opacity: 1, scale: 1 }}
                  className="rounded-2xl bg-canvas p-3 ring-1 ring-stone/60">
                  <div className="eyebrow">{m.label}</div>
                  <div className="num mt-1 text-sm break-words">{m.value} <span className="text-xs text-muted-foreground">{m.unit}</span></div>
                </motion.div>
              ))}
            </div>
          )}

          {raws.length > 0 && (
            <Table>
              <TableHeader>
                <TableRow>
                  <TableHead>Duty A/B/C %</TableHead>
                  <TableHead className="text-right">iA</TableHead><TableHead className="text-right">iB</TableHead>
                  <TableHead className="text-right">iC</TableHead><TableHead className="text-right">Flow A/B/C %</TableHead>
                </TableRow>
              </TableHeader>
              <TableBody>
                {raws.map((r, k) => {
                  const d = r.d as number[], m = r.mean as number[], f = r.flow as number[]
                  return (
                    <TableRow key={k}>
                      <TableCell className="num">{d.join(' / ')}</TableCell>
                      {m.map((x, j) => <TableCell key={j} className="num text-right">{x >= 0 ? '+' : ''}{x.toFixed(2)} A</TableCell>)}
                      <TableCell className="num text-right">{f.join(' / ')}</TableCell>
                    </TableRow>
                  )
                })}
              </TableBody>
            </Table>
          )}

          {others.length > 0 && (
            <div className="flex flex-wrap gap-3">
              {others.map((r, k) => <ResultChip key={k} r={r} />)}
            </div>
          )}

          <ScrollArea className="h-48 rounded-2xl bg-slate p-4">
            <pre className="num text-[11.5px] leading-relaxed whitespace-pre-wrap text-ivory/90">
              {job.log.length ? job.log.join('\n') : 'waiting for output…'}
            </pre>
            <div ref={logEnd} />
          </ScrollArea>
        </CardContent>
      </Card>
    </motion.div>
  )
}

function ResultChip({ r }: { r: Record<string, unknown> & { kind: string } }) {
  let title = '', body = ''
  if (r.kind === 'rl') {
    title = 'R / L'
    body = r.ok ? `R ${fmt(r.R as number, 4)} Ω · L ${fmt(r.L as number, 1)} µH (1 kHz ${fmt(r.L1k as number, 1)}, 2 kHz ${fmt(r.L2k as number, 1)})` : `failed: ${r.why}`
  } else if (r.kind === 'csa') {
    title = 'CSA offsets'
    body = (r.off as number[]).map((v, k) => `SO${'ABC'[k]} ${v.toFixed(3)} V`).join(' · ')
  } else if (r.kind === 'result') {
    title = `Run · ${r.start === 'start' ? r.kind : r.start}`
    body = `${r.why} · max ${fmt(r.fmax as number, 1)} Hz · ${fmt(r.imax as number)} A`
  }
  return (
    <div className="rounded-2xl bg-canvas px-4 py-3 ring-1 ring-stone/60">
      <div className="eyebrow">{title}</div>
      <div className="num mt-1 text-xs">{body}</div>
    </div>
  )
}
