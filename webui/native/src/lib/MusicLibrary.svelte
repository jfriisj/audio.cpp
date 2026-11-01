<script lang="ts">
  import { onMount } from 'svelte';
  import { Download, Music2, RefreshCw, RotateCcw, Search } from '@lucide/svelte';
  import { listGenerationJobs, retryGenerationJob } from './api';
  import type { GenerationJob } from './types';

  type JobStatus = GenerationJob['status'];
  type Filter = 'all' | JobStatus;

  const filters: { id: Filter; label: string }[] = [
    { id: 'all', label: 'All' },
    { id: 'running', label: 'Running' },
    { id: 'queued', label: 'Queued' },
    { id: 'completed', label: 'Completed' },
    { id: 'failed', label: 'Failed' },
    { id: 'interrupted', label: 'Interrupted' }
  ];

  let jobs: GenerationJob[] = [];
  let loading = true;
  let refreshing = false;
  let error = '';
  let retrying = '';
  let filter: Filter = 'all';
  let query = '';
  let lastUpdated = 0;

  function prompt(job: GenerationJob): string {
    try {
      const data = JSON.parse(job.request_json) as {
        request?: { text?: string; lyrics?: string };
      };
      return data.request?.text || data.request?.lyrics || 'Music generation';
    } catch {
      return 'Music generation';
    }
  }

  function created(job: GenerationJob): string {
    return new Date(job.created_at_ms).toLocaleString();
  }

  function statusLabel(status: JobStatus): string {
    return status === 'running' ? 'Generating'
      : status === 'queued' ? 'In queue'
      : status === 'completed' ? 'Completed'
      : status === 'failed' ? 'Failed'
      : 'Interrupted';
  }

  function count(status: Filter): number {
    return status === 'all' ? jobs.length : jobs.filter(job => job.status === status).length;
  }

  $: matchingJobs = jobs
    .filter(job => filter === 'all' || job.status === filter)
    .filter(job => {
      const searchable = [prompt(job), job.model, job.id].join(' ').toLowerCase();
      return searchable.includes(query.trim().toLowerCase());
    })
    .sort((a, b) => b.created_at_ms - a.created_at_ms);

  $: activeCount = jobs.filter(job => job.status === 'queued' || job.status === 'running').length;
  $: completedCount = count('completed');
  $: attentionCount = count('failed') + count('interrupted');

  async function refresh(silent = false): Promise<void> {
    if (refreshing) return;
    refreshing = true;
    if (!silent && !jobs.length) loading = true;
    try {
      jobs = await listGenerationJobs();
      lastUpdated = Date.now();
      error = '';
    } catch (cause) {
      error = cause instanceof Error ? cause.message : String(cause);
    } finally {
      refreshing = false;
      loading = false;
    }
  }

  async function retry(job: GenerationJob): Promise<void> {
    if (retrying) return;
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
    const interval = window.setInterval(() => void refresh(true), 5000);
    return () => window.clearInterval(interval);
  });
</script>

<section class="music-workspace" aria-label="Music generation library">
  <header class="music-header">
    <div class="music-title">
      <span class="music-eyebrow"><Music2 size={15} aria-hidden="true" /> GENERATION WORKSPACE</span>
      <h1>Music Library</h1>
      <p>Your music generations, stored on the server. Leave this page and return whenever you like.</p>
    </div>
    <div class="music-header-actions">
      {#if lastUpdated}
        <span class="music-updated">Updated {new Date(lastUpdated).toLocaleTimeString()}</span>
      {/if}
      <button class="music-refresh" type="button" disabled={refreshing} on:click={() => void refresh()} aria-label="Refresh music generations">
        <RefreshCw size={16} class:spinning={refreshing} aria-hidden="true" />
        {refreshing ? 'Refreshing' : 'Refresh'}
      </button>
    </div>
  </header>

  <div class="music-summary" aria-label="Generation summary">
    <div class="music-metric"><span>All generations</span><strong>{jobs.length}</strong><small>Saved jobs</small></div>
    <div class="music-metric"><span>In progress</span><strong>{activeCount}</strong><small>Queued or generating</small></div>
    <div class="music-metric"><span>Completed</span><strong>{completedCount}</strong><small>Ready to listen</small></div>
    <div class="music-metric"><span>Needs attention</span><strong>{attentionCount}</strong><small>Failed or interrupted</small></div>
  </div>

  <div class="music-library-panel">
    <div class="music-toolbar">
      <div class="music-toolbar-title">
        <h2>Generations</h2>
        <span>{matchingJobs.length} shown</span>
      </div>
      <div class="music-search">
        <Search size={17} aria-hidden="true" />
        <input type="search" aria-label="Search music generations" placeholder="Search prompts or models" bind:value={query} />
      </div>
    </div>

    <div class="music-filters" role="group" aria-label="Filter by generation status">
      {#each filters as option (option.id)}
        <button type="button" class:active={filter === option.id} aria-pressed={filter === option.id} on:click={() => filter = option.id}>
          {option.label} <span>{count(option.id)}</span>
        </button>
      {/each}
    </div>

    {#if error}
      <div class="music-error" role="alert">
        <strong>Unable to update the library.</strong>
        <span>{error}</span>
        <button type="button" on:click={() => void refresh()}>Try again</button>
      </div>
    {/if}

    {#if loading}
      <div class="music-empty" role="status">Loading your music library…</div>
    {:else if jobs.length === 0}
      <div class="music-empty">
        <Music2 size={32} aria-hidden="true" />
        <h3>Your music starts here</h3>
        <p>No generations yet. Choose a music model in Studio and start a generation.</p>
      </div>
    {:else if matchingJobs.length === 0}
      <div class="music-empty">
        <Search size={30} aria-hidden="true" />
        <h3>No matching generations</h3>
        <p>Try another search or filter.</p>
        <button type="button" on:click={() => { filter = 'all'; query = ''; }}>Clear filters</button>
      </div>
    {:else}
      <div class="music-job-list" aria-live="polite">
        {#each matchingJobs as job (job.id)}
          <article class="music-job-card">
            <div class="music-job-main">
              <div class="music-job-icon"><Music2 size={22} aria-hidden="true" /></div>
              <div class="music-job-info">
                <h3 title={prompt(job)}>{prompt(job)}</h3>
                <p>{job.model} <span aria-hidden="true">·</span> {created(job)}</p>
              </div>
              <span class="music-status" data-status={job.status}>
                <span class="music-status-dot" aria-hidden="true"></span>{statusLabel(job.status)}
              </span>
            </div>

            {#if job.status === 'queued' || job.status === 'running'}
              <div class="music-job-message">
                {job.status === 'queued' ? 'Waiting for a GPU worker.' : 'Generating on the server. You can safely leave this page.'}
              </div>
            {:else if job.status === 'failed' || job.status === 'interrupted'}
              <div class="music-job-failure">
                <p>{job.error || 'This generation did not finish.'}</p>
                <button type="button" disabled={Boolean(retrying)} on:click={() => void retry(job)}>
                  <RotateCcw size={15} aria-hidden="true" />
                  {retrying === job.id ? 'Retrying…' : 'Retry generation'}
                </button>
              </div>
            {:else if job.status === 'completed'}
              {#if job.audio_urls.length}
                <div class="music-tracks">
                  {#each job.audio_urls as url, index}
                    <div class="music-track">
                      <span class="music-track-name">Track {index + 1}</span>
                      <audio controls preload="none" src={url} aria-label={'Play track ' + (index + 1)}></audio>
                      <a class="music-download" href={url} download={job.id + '-' + index + '.wav'} aria-label={'Download track ' + (index + 1) + ' as WAV'}>
                        <Download size={16} aria-hidden="true" /><span>WAV</span>
                      </a>
                    </div>
                  {/each}
                </div>
              {:else}
                <p class="music-job-message">Generation finished, but no audio files were returned.</p>
              {/if}
            {/if}
          </article>
        {/each}
      </div>
    {/if}
  </div>
</section>

<style>
  .music-workspace { display: grid; gap: 18px; min-width: 0; padding: 6px 0 40px; }
  .music-header { display: flex; align-items: end; justify-content: space-between; gap: 20px; }
  .music-title { min-width: 0; }
  .music-eyebrow { display: inline-flex; align-items: center; gap: 8px; margin-bottom: 12px; color: var(--cyan); font-size: 10px; font-weight: 800; letter-spacing: .14em; }
  .music-title h1 { margin: 0 0 10px; font-size: clamp(30px, 3.5vw, 46px); letter-spacing: -.05em; }
  .music-title p { max-width: 620px; margin: 0; color: var(--muted); font-size: 13px; line-height: 1.6; }
  .music-header-actions { display: flex; align-items: center; gap: 12px; flex: 0 0 auto; }
  .music-updated { font-size: 11px; color: var(--muted); }
  .music-refresh, .music-job-failure button { display: inline-flex; align-items: center; justify-content: center; gap: 8px; min-height: 38px; }
  .spinning { animation: spin 1.2s linear infinite; }
  .music-summary { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 12px; }
  .music-metric { display: flex; flex-direction: column; gap: 8px; padding: 18px 20px; min-width: 0; border: 1px solid var(--line); border-radius: 12px; background: var(--panel-bg); }
  .music-metric span { color: var(--text-label); font-size: 12px; font-weight: 600; }
  .music-metric strong { color: var(--text-strong); font-size: 32px; letter-spacing: -.05em; line-height: 1; font-variant-numeric: tabular-nums; }
  .music-metric small { color: var(--muted); font-size: 11px; }
  .music-library-panel { min-width: 0; border: 1px solid var(--line); border-radius: 13px; background: var(--panel-bg); overflow: hidden; }
  .music-toolbar { display: flex; align-items: center; justify-content: space-between; gap: 16px; padding: 21px 22px 14px; }
  .music-toolbar-title { display: flex; align-items: baseline; gap: 12px; }
  .music-toolbar-title h2 { margin: 0; color: var(--text-strong); font-size: 18px; letter-spacing: -.02em; }
  .music-toolbar-title span { color: var(--muted); font-size: 12px; white-space: nowrap; }
  .music-search { display: flex; align-items: center; gap: 8px; width: min(330px, 100%); min-height: 40px; padding: 0 12px; border: 1px solid var(--control-border); border-radius: 9px; color: var(--muted); background: var(--control-bg); }
  .music-search:focus-within { border-color: var(--cyan); }
  .music-search input { width: 100%; min-width: 0; padding: 8px 0; border: 0; box-shadow: none; background: transparent; color: var(--text); font-size: 12px; outline: none; }
  .music-search input:focus { box-shadow: none; border: 0; }
  .music-filters { display: flex; flex-wrap: wrap; gap: 7px; padding: 0 22px 18px; border-bottom: 1px solid var(--line); }
  .music-filters button { display: inline-flex; align-items: center; gap: 7px; min-height: 32px; padding: 6px 10px; border: 1px solid var(--line); border-radius: 8px; color: var(--muted); background: var(--control-bg); font-size: 11px; }
  .music-filters button.active { color: var(--cyan); border-color: var(--active-border); background: var(--active-bg); }
  .music-filters span { padding: 1px 5px; border-radius: 5px; color: inherit; background: var(--chip-bg); font-variant-numeric: tabular-nums; }
  .music-job-list { display: grid; gap: 12px; padding: 18px 22px 22px; }
  .music-job-card { display: grid; gap: 15px; min-width: 0; padding: 17px; border: 1px solid var(--line); border-radius: 11px; background: var(--card-bg); }
  .music-job-main { display: flex; align-items: center; gap: 13px; min-width: 0; }
  .music-job-icon { flex: 0 0 44px; width: 44px; height: 44px; display: grid; place-items: center; border: 1px solid var(--line); border-radius: 10px; color: var(--cyan); background: var(--icon-bg); }
  .music-job-info { min-width: 0; flex: 1 1 auto; }
  .music-job-info h3 { margin: 0 0 6px; color: var(--text-strong); font-size: 14px; font-weight: 700; line-height: 1.45; overflow-wrap: anywhere; }
  .music-job-info p { margin: 0; color: var(--muted); font-size: 11px; line-height: 1.5; }
  .music-status { display: inline-flex; align-items: center; gap: 7px; flex: 0 0 auto; padding: 6px 10px; border: 1px solid var(--line); border-radius: 999px; color: var(--muted); background: var(--panel); font-size: 11px; font-weight: 700; }
  .music-status-dot { width: 7px; height: 7px; border-radius: 50%; background: currentColor; }
  .music-status[data-status="completed"] { color: var(--cyan); border-color: var(--active-border); }
  .music-status[data-status="running"] { color: var(--blue); }
  .music-status[data-status="failed"], .music-status[data-status="interrupted"] { color: var(--danger); }
  .music-job-message { margin: 0; color: var(--muted); font-size: 12px; line-height: 1.6; }
  .music-job-failure { display: flex; flex-wrap: wrap; align-items: center; justify-content: space-between; gap: 12px; }
  .music-job-failure p { flex: 1 1 220px; margin: 0; color: var(--danger); font-size: 12px; overflow-wrap: anywhere; }
  .music-tracks { display: grid; gap: 9px; }
  .music-track { display: flex; align-items: center; gap: 12px; min-width: 0; padding: 11px 12px; border: 1px solid var(--line); border-radius: 9px; background: var(--panel); }
  .music-track-name { flex: 0 0 auto; min-width: 54px; color: var(--text-label); font-size: 11px; font-weight: 700; }
  .music-track audio { flex: 1 1 auto; width: 100%; min-width: 110px; height: 37px; }
  .music-download { display: inline-flex; justify-content: center; align-items: center; gap: 7px; flex: 0 0 auto; min-height: 36px; padding: 7px 12px; border: 1px solid var(--line); border-radius: 8px; color: var(--text); background: var(--button-bg); font-size: 11px; font-weight: 700; text-decoration: none; }
  .music-download:hover { border-color: var(--cyan); color: var(--cyan); }
  .music-error { display: flex; align-items: center; flex-wrap: wrap; gap: 10px; margin: 16px 22px 0; padding: 12px; border: 1px solid var(--danger); border-radius: 9px; font-size: 12px; }
  .music-error strong { color: var(--danger); }
  .music-error span { flex: 1 1 180px; overflow-wrap: anywhere; }
  .music-empty { display: grid; place-items: center; justify-items: center; gap: 10px; min-height: 220px; padding: 45px 24px; color: var(--muted); text-align: center; }
  .music-empty h3 { margin: 0; color: var(--text-strong); font-size: 17px; }
  .music-empty p { max-width: 410px; margin: 0; font-size: 12px; line-height: 1.6; }
  @keyframes spin { to { transform: rotate(360deg); } }
  @media (max-width: 900px) {
    .music-summary { grid-template-columns: repeat(2, minmax(0, 1fr)); }
    .music-header { align-items: start; flex-direction: column; }
  }
  @media (max-width: 650px) {
    .music-summary { gap: 8px; }
    .music-metric { padding: 14px; }
    .music-toolbar { flex-direction: column; align-items: stretch; padding: 16px 14px 12px; }
    .music-search { width: 100%; }
    .music-filters { padding: 0 14px 14px; }
    .music-job-list { padding: 14px; }
    .music-job-card { padding: 13px; }
    .music-job-main { flex-wrap: wrap; }
    .music-status { margin-left: 57px; }
    .music-track { flex-wrap: wrap; }
    .music-track audio { order: 3; flex-basis: 100%; }
    .music-download { margin-left: auto; }
  }
  @media (prefers-reduced-motion: reduce) { .spinning { animation: none; } }
</style>
