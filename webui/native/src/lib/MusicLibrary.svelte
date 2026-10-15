<script lang="ts">
  import { onMount } from 'svelte';
  import { listGenerationJobs, retryGenerationJob } from './api';
  import type { GenerationJob } from './types';

  let jobs: GenerationJob[] = [];
  let loading = true;
  let error = '';
  let retrying = '';

  function prompt(job: GenerationJob): string {
    try {
      const data = JSON.parse(job.request_json) as { request?: { text?: string; lyrics?: string } };
      return data.request?.text || data.request?.lyrics || 'Music generation';
    } catch {
      return 'Music generation';
    }
  }

  function created(job: GenerationJob): string {
    return new Date(job.created_at_ms).toLocaleString();
  }

  async function refresh() {
    try {
      jobs = await listGenerationJobs();
      error = '';
    } catch (cause) {
      error = cause instanceof Error ? cause.message : String(cause);
    } finally {
      loading = false;
    }
  }

  async function retry(job: GenerationJob) {
    retrying = job.id;
    try {
      await retryGenerationJob(job.id);
      await refresh();
    } catch (cause) {
      error = cause instanceof Error ? cause.message : String(cause);
    } finally {
      retrying = '';
    }
  }

  onMount(() => {
    void refresh();
    const interval = window.setInterval(() => void refresh(), 5000);
    return () => window.clearInterval(interval);
  });
</script>

<section class="page-head">
  <p class="eyebrow">Generation queue</p>
  <h1>Music Library</h1>
  <p>Music is generated on the server. You can close this page and return later.</p>
</section>

<section class="panel music-library">
  <div class="music-library-heading">
    <h2>Generations ({jobs.length})</h2>
    <button on:click={() => void refresh()}>Refresh</button>
  </div>
  {#if error}
    <p class="music-job-error" role="alert">{error}</p>
  {/if}
  {#if loading}
    <p>Loading generations…</p>
  {:else if jobs.length === 0}
    <p>No generations yet. Select a music model in Studio and press Generate.</p>
  {/if}
  {#each jobs as job (job.id)}
    <article class="music-job">
      <div class="music-job-header">
        <div>
          <strong>{prompt(job)}</strong>
          <small>{job.model} · {created(job)}</small>
        </div>
        <span class="music-job-status" data-status={job.status}>{job.status}</span>
      </div>
      {#if job.status === 'running'}
        <p>Generating on GPU… You can leave this page.</p>
      {:else if job.status === 'queued'}
        <p>Waiting for GPU worker…</p>
      {:else if job.status === 'failed' || job.status === 'interrupted'}
        <p class="music-job-error">{job.error || 'Generation did not finish.'}</p>
        <button disabled={retrying === job.id} on:click={() => void retry(job)}>
          {retrying === job.id ? 'Retrying…' : 'Retry generation'}
        </button>
      {:else if job.status === 'completed'}
        {#each job.audio_urls as url, index}
          <div class="music-job-audio">
            <audio controls preload="none" src={url}></audio>
            <a href={url} download={job.id + '-' + index + '.wav'}>Download WAV {index + 1}</a>
          </div>
        {/each}
      {/if}
    </article>
  {/each}
</section>
