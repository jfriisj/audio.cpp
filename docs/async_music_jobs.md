# Asynchronous music generation

This fork adds **server-side queued music generation** without changing the synchronous
audio.cpp endpoints. The native WebUI remains a Svelte/TypeScript frontend embedded
in the C++ server. No Python service, Redis, or separate web server is required.

## Docker (repository-owned setup)

Run these commands from the root of this fork:

    mkdir -p models output/generations
    sudo chown -R 1000:1000 models output
    docker compose up -d --build


Open http://localhost:8080 on the Docker host.

For remote access, create a Cloudflare Tunnel and configure its public hostname
(e.g. music.friis-jakobsen.dk) to point to **http://audiocpp:8080**.
Protect it with Cloudflare Access before exposing the model manager. Copy
.env.example to .env, set CLOUDFLARE_TUNNEL_TOKEN, and run:

    docker compose --profile remote up -d --build


The Cloudflare connector is optional. It is not needed for local use.

The Compose build retains the upstream CUDA Dockerfile and its full
target. The Dockerfile now builds Svelte with Node in an intermediate stage
**before** CMake embeds webui/native/dist/index.html. Node is not installed
in the runtime image.

> The Dockerfile runs the server as Ubuntu's unprivileged ubuntu user
> (typically UID 1000); ensure the bind-mounted output directory is writable.

## Behavior

In Studio, for **text/lyrics-only** music generation (task=gen), Generate
queues the request and opens the **Music Library** tab. The backend optionally
loads/reconfigures the selected model **inside the worker**, then runs the
existing model inference. Progress is shown as queued / running /
completed. The browser polls every 5 seconds. It can be closed
without cancelling the job.

Only one queued generation is executed at a time. Existing synchronous
generation endpoints remain unchanged for other tasks and clients.

WAV outputs and metadata live in:

    output/generations/<id>.json
    output/generations/<id>-0.wav
    output/generations/<id>-1.wav  # if the model returned multiple tracks


The JSON stores the input request (including model load settings), timestamps,
status, and errors. On an unclean restart, previously queued/running jobs are
marked interrupted, not replayed automatically. Retry is available after
models/inputs are accessible. Completed WAV files remain available after a
container restart. Initial support excludes audio input/reference uploads
because the server's temporary upload directory is not restart-persistent.

## Endpoints

    POST /v1/tasks/jobs
    GET  /v1/tasks/jobs
    GET  /v1/tasks/jobs/<id>
    POST /v1/tasks/jobs/<id>/retry
    GET  /v1/tasks/jobs/<id>/audio/<index>


Example:

    curl -s -X POST http://127.0.0.1:8080/v1/tasks/jobs \
      -H "Content-Type: application/json" \
      -d '{"model":"your-registered-model","request":{"text":"Nordic folk music","duration_seconds":30}}'

    curl -s http://127.0.0.1:8080/v1/tasks/jobs


Successful submission returns HTTP 202 immediately. Internal model loading and
music generation run off-request so Cloudflare's origin HTTP timeout does not
apply to the long-running inference. Downloading a large WAV is still a
separate HTTP request.

## Tests

The C++ test server_generation_jobs_test exercises job creation, saved WAV
files, listing, path validation, restart recovery, and retry without a GPU.
With CMake tests enabled:

    cmake -S . -B build -DENGINE_ENABLE_CUDA=OFF -DENGINE_BUILD_TESTS=ON
    cmake --build build --target server_generation_jobs_test
    ctest --test-dir build -R server_generation_jobs_test --output-on-failure


Manual integration checklist:

1. Start a 3+ minute ACE-Step generation via the remote URL. Verify POST /v1/tasks/jobs
   returns HTTP 202 quickly; no Cloudflare 524 should appear from this POST.
2. Navigate away or close the browser; reopen Music Library, verify active status.
3. Let it finish, play and download all generated WAV outputs.
4. Restart the container; the completed track and metadata must remain.
5. Interrupt a running generation with docker compose kill audiocpp; restart,
   verify it is marked interrupted and can be retried.
6. Queue different model configurations with --max-loaded-models 1 and
   confirm they run sequentially.

Do not expose the UI management or generation API publicly without
Cloudflare Access or equivalent authentication. API calls operate with
the same privileges as the server's WebUI.
