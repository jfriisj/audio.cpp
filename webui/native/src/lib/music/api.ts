import { jsonRequest } from '../api';
import type { GenerationJob } from './types';

export async function createGenerationJob(body: Record<string, unknown>): Promise<GenerationJob> {
  return jsonRequest<GenerationJob>('/v1/tasks/jobs', {
    method: 'POST',
    body: JSON.stringify(body)
  });
}

export async function listGenerationJobs(): Promise<GenerationJob[]> {
  const response = await jsonRequest<{ data: GenerationJob[] }>('/v1/tasks/jobs');
  return response.data;
}

export async function retryGenerationJob(id: string): Promise<GenerationJob> {
  return jsonRequest<GenerationJob>('/v1/tasks/jobs/' + encodeURIComponent(id) + '/retry', { method: 'POST' });
}
