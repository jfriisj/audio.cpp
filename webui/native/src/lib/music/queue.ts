import type { CatalogEntry, StringMap } from '../types';
import { createGenerationJob } from './api';
import type { GenerationJob } from './types';

export interface MusicQueueOptions {
  model: CatalogEntry;
  modelPath: string;
  importedLoadOptions: StringMap;
  sessionOptions: StringMap;
  includeModelLoad: boolean;
}

// Keep the request envelope and optional model-load payload independent of Studio.
export function queueMusicGeneration(
  body: { model: string; request: Record<string, unknown> },
  { model, modelPath, importedLoadOptions, sessionOptions, includeModelLoad }: MusicQueueOptions
): Promise<GenerationJob> {
  return createGenerationJob({
    ...body,
    ...(includeModelLoad ? {
      model_load: {
        id: model.id,
        path: modelPath,
        family: model.family,
        task: model.task,
        mode: model.mode || 'offline',
        load_options: { ...(model.load_options || {}), ...importedLoadOptions },
        session_options: sessionOptions
      }
    } : {})
  });
}
