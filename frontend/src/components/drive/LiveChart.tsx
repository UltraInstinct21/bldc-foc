import { CartesianGrid, Line, LineChart, ReferenceLine, XAxis, YAxis } from 'recharts'
import { ChartContainer, ChartTooltip, ChartTooltipContent, type ChartConfig } from '@/components/ui/chart'
import { cn } from '@/lib/utils'

export interface Series {
  key: string
  label: string
  color: string
  dashed?: boolean
  get?: (d: any) => number // eslint-disable-line @typescript-eslint/no-explicit-any
}

/**
 * Streaming line chart (shadcn chart = Recharts). One y-axis only; series of
 * different units go in separate charts. Legend row above + value labels at
 * the right edge, so identity never rests on colour alone.
 */
export function LiveChart({
  data, series, unit, x = 'ts', xUnit = 's', height = 200, domain, zero, className, digits = 2,
}: {
  data: any[] // eslint-disable-line @typescript-eslint/no-explicit-any
  series: Series[]
  unit: string
  x?: string
  xUnit?: string
  height?: number
  domain?: [number | 'auto' | 'dataMin' | 'dataMax', number | 'auto' | 'dataMin' | 'dataMax']
  zero?: boolean
  className?: string
  digits?: number
}) {
  const config: ChartConfig = Object.fromEntries(series.map((s) => [s.key, { label: s.label, color: s.color }]))
  const rows = data.map((d) => {
    const r: Record<string, number> = { [x]: d[x] }
    for (const s of series) r[s.key] = s.get ? s.get(d) : d[s.key]
    return r
  })
  const last = rows[rows.length - 1]
  const span = rows.length > 1 ? rows[rows.length - 1][x] - rows[0][x] : 0
  const xDigits = span < 2 ? 2 : span < 20 ? 1 : 0

  return (
    <div className={cn('flex flex-col gap-2', className)}>
      <div className="flex flex-wrap items-center gap-x-4 gap-y-1">
        <span className="eyebrow mr-1">{unit}</span>
          {series.length > 1 && series.map((s) => (
            <span key={s.key} className="inline-flex items-center gap-1.5 font-sans text-[12px] text-slate-medium">
              <svg width="16" height="6" aria-hidden>
                <line x1="1" y1="3" x2="15" y2="3" stroke={s.color} strokeWidth="2" strokeLinecap="round"
                  strokeDasharray={s.dashed ? '3 3' : undefined} />
              </svg>
              {s.label}
              {last && Number.isFinite(last[s.key]) && (
                <span className="num text-foreground">{last[s.key].toFixed(digits)}</span>
              )}
            </span>
          ))}
      </div>
      <ChartContainer config={config} className="aspect-auto w-full [&_.recharts-cartesian-axis-tick_text]:font-mono [&_.recharts-cartesian-axis-tick_text]:text-[10.5px]" style={{ height }}>
        <LineChart data={rows} margin={{ top: 6, right: 8, bottom: 0, left: 0 }}>
          <CartesianGrid vertical={false} stroke="var(--color-stone)" strokeOpacity={0.6} />
          <XAxis
            dataKey={x}
            type="number"
            domain={['dataMin', 'dataMax']}
            tickLine={false}
            axisLine={false}
            tickMargin={6}
            minTickGap={40}
            tickFormatter={(v: number) => `${v.toFixed(xDigits)} ${xUnit}`}
          />
          <YAxis
            width={44}
            tickLine={false}
            axisLine={false}
            domain={domain ?? ['auto', 'auto']}
            tickFormatter={(v: number) => `${+v.toFixed(2)}`}
          />
          {zero && <ReferenceLine y={0} stroke="var(--color-cloud)" />}
          <ChartTooltip
            cursor={{ stroke: 'var(--color-cloud-dark)', strokeWidth: 1 }}
            content={<ChartTooltipContent indicator="line" labelFormatter={(_, p) =>
              p?.[0] ? `${Number(p[0].payload[x]).toFixed(xUnit === 'ms' ? 2 : 1)} ${xUnit}` : ''} />}
          />
          {series.map((s) => (
            <Line
              key={s.key}
              dataKey={s.key}
              type="monotone"
              stroke={s.color}
              strokeWidth={2}
              strokeDasharray={s.dashed ? '4 4' : undefined}
              dot={false}
              activeDot={{ r: 4, strokeWidth: 2, stroke: 'var(--color-ivory)' }}
              isAnimationActive={false}
            />
          ))}
        </LineChart>
      </ChartContainer>
    </div>
  )
}
