// Types exclusive to the optional server-side music generation queue.
export interface GenerationJob {
  id: string;
  status: 'queued' | 'running' | 'completed' | 'failed' | 'interrupted';
  model: string;
  request_json: string;
  error: string;
  created_at_ms: number;
  started_at_ms: number;
  finished_at_ms: number;
  output_count: number;
  audio_urls: string[];
}
