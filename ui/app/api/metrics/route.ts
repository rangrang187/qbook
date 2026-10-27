import type { NextRequest } from 'next/server';
import { createMetricsResponse } from '../../../lib/server/metrics-stream';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

export async function GET(request: NextRequest): Promise<Response> {
  try {
    return createMetricsResponse(request.signal);
  } catch (error) {
    console.error('metrics subscription setup failed', error);
    return new Response('Metrics subscription unavailable\n', {
      status: 503,
      headers: { 'Cache-Control': 'no-store', 'Content-Type': 'text/plain; charset=utf-8' },
    });
  }
}
