#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define YOLO_ENABLED 1
#define YOLO_PYTHON_EXE "python"
#define YOLO_MODEL_PATH "best.pt"
#define YOLO_WORKER_SCRIPT "src\\yolo_worker.py"
#define YOLO_INPUT_SIZE 320
#define YOLO_CONFIDENCE 0.90f
#define YOLO_FRAME_INTERVAL_MS 33
#define YOLO_MAX_DETECTIONS 1

#define CAMERA_TARGET_WIDTH 1920
#define CAMERA_TARGET_HEIGHT 1080
#define CAMERA_TARGET_FPS 60

#define AIM_POINT_X_RATIO (1.0f / 1.82f)
#define AIM_POINT_Y_RATIO (1.0f / 2.40f)

#define AUTO_AIM_ENABLED 1
#define AUTO_AIM_CENTER_ZONE_RATIO 0.80f
#define AUTO_AIM_MAX_OUTPUT_PERCENT 45.0f
#define AUTO_AIM_INVERT_X 0
#define AUTO_AIM_INVERT_Y 0

#define AUTO_AIM_X_KP 70.0f
#define AUTO_AIM_X_KI 0.0f
#define AUTO_AIM_X_KD 8.0f
#define AUTO_AIM_Y_KP 70.0f
#define AUTO_AIM_Y_KI 0.0f
#define AUTO_AIM_Y_KD 8.0f

static int clamp_int(int value, int min_value, int max_value);

typedef struct
{
    int class_id;
    float confidence;
    float x[4];
    float y[4];
} YoloDetection;

typedef struct
{
    int has_detection;
    int model_ready;
    int frame_width;
    int frame_height;
    YoloDetection detection;
} YoloSnapshot;

typedef struct
{
    float kp;
    float ki;
    float kd;
    float integral;
    float previous_error;
    int has_previous_error;
    Uint64 previous_ticks_ms;
} PidController;

typedef struct
{
    PidController x;
    PidController y;
    int active_last_frame;
} AutoAimState;

#ifdef _WIN32
typedef struct
{
    HANDLE process;
    HANDLE stdin_write;
    HANDLE stdout_read;
    SDL_Thread *thread;
    SDL_Mutex *mutex;
    SDL_Condition *condition;
    unsigned char *pending_frame;
    size_t pending_capacity;
    size_t pending_size;
    int pending_width;
    int pending_height;
    int has_pending;
    int stop;
    int enabled;
    YoloDetection detections[YOLO_MAX_DETECTIONS];
    int detection_count;
    int frame_width;
    int frame_height;
    int model_ready;
    char status[160];
} YoloWorker;

static void yolo_worker_set_status(YoloWorker *worker, const char *status)
{
    if (!worker || !worker->mutex)
    {
        return;
    }

    SDL_LockMutex(worker->mutex);
    snprintf(worker->status, sizeof(worker->status), "%s", status);
    SDL_UnlockMutex(worker->mutex);
}

static int write_exact(HANDLE handle, const void *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)data;

    while (size > 0)
    {
        DWORD chunk = size > (size_t)(1024 * 1024) ? (DWORD)(1024 * 1024) : (DWORD)size;
        DWORD written = 0;

        if (!WriteFile(handle, bytes, chunk, &written, NULL) || written == 0)
        {
            return 0;
        }

        bytes += written;
        size -= written;
    }

    return 1;
}

static int read_protocol_line(HANDLE handle, char *buffer, size_t capacity)
{
    size_t used = 0;

    if (capacity == 0)
    {
        return 0;
    }

    while (used + 1 < capacity)
    {
        char c = '\0';
        DWORD read = 0;

        if (!ReadFile(handle, &c, 1, &read, NULL) || read == 0)
        {
            return 0;
        }

        if (c == '\n')
        {
            break;
        }

        if (c != '\r')
        {
            buffer[used++] = c;
        }
    }

    buffer[used] = '\0';
    return 1;
}

static int yolo_worker_thread(void *data)
{
    YoloWorker *worker = (YoloWorker *)data;
    unsigned char *local_frame = NULL;
    size_t local_capacity = 0;

    for (;;)
    {
        int width = 0;
        int height = 0;
        size_t frame_size = 0;

        SDL_LockMutex(worker->mutex);
        while (!worker->has_pending && !worker->stop)
        {
            SDL_WaitCondition(worker->condition, worker->mutex);
        }

        if (worker->stop)
        {
            SDL_UnlockMutex(worker->mutex);
            break;
        }

        width = worker->pending_width;
        height = worker->pending_height;
        frame_size = worker->pending_size;

        if (frame_size > local_capacity)
        {
            unsigned char *resized = (unsigned char *)realloc(local_frame, frame_size);
            if (!resized)
            {
                worker->has_pending = 0;
                snprintf(worker->status, sizeof(worker->status), "YOLO: allocation frame impossible");
                SDL_UnlockMutex(worker->mutex);
                continue;
            }

            local_frame = resized;
            local_capacity = frame_size;
        }

        memcpy(local_frame, worker->pending_frame, frame_size);
        worker->has_pending = 0;
        SDL_UnlockMutex(worker->mutex);

        uint32_t header[3] = {(uint32_t)width, (uint32_t)height, (uint32_t)frame_size};
        if (!write_exact(worker->stdin_write, header, sizeof(header)) ||
            !write_exact(worker->stdin_write, local_frame, frame_size))
        {
            yolo_worker_set_status(worker, "YOLO: ecriture vers le worker echouee");
            break;
        }

        char line[512];
        if (!read_protocol_line(worker->stdout_read, line, sizeof(line)))
        {
            yolo_worker_set_status(worker, "YOLO: worker arrete");
            break;
        }

        int reported_count = atoi(line);
        int detection_count = clamp_int(reported_count, 0, YOLO_MAX_DETECTIONS);
        YoloDetection detections[YOLO_MAX_DETECTIONS];
        memset(detections, 0, sizeof(detections));

        for (int i = 0; i < reported_count; ++i)
        {
            if (!read_protocol_line(worker->stdout_read, line, sizeof(line)))
            {
                yolo_worker_set_status(worker, "YOLO: lecture resultats echouee");
                reported_count = i;
                detection_count = clamp_int(reported_count, 0, YOLO_MAX_DETECTIONS);
                break;
            }

            if (i >= YOLO_MAX_DETECTIONS)
            {
                continue;
            }

            YoloDetection detection;
            memset(&detection, 0, sizeof(detection));
            int parsed = sscanf(line, "%d %f %f %f %f %f %f %f %f %f",
                                &detection.class_id,
                                &detection.confidence,
                                &detection.x[0], &detection.y[0],
                                &detection.x[1], &detection.y[1],
                                &detection.x[2], &detection.y[2],
                                &detection.x[3], &detection.y[3]);
            if (parsed == 10)
            {
                detections[i] = detection;
            }
        }

        SDL_LockMutex(worker->mutex);
        memcpy(worker->detections, detections, sizeof(detections));
        worker->detection_count = detection_count;
        worker->frame_width = width;
        worker->frame_height = height;
        worker->model_ready = 1;
        snprintf(worker->status, sizeof(worker->status), "YOLO: %d detection(s), input %d", detection_count, YOLO_INPUT_SIZE);
        SDL_UnlockMutex(worker->mutex);
    }

    free(local_frame);
    return 0;
}

static int yolo_worker_start(YoloWorker *worker)
{
    memset(worker, 0, sizeof(*worker));
    snprintf(worker->status, sizeof(worker->status), "YOLO: inactif");

    if (!YOLO_ENABLED)
    {
        return 0;
    }

    worker->mutex = SDL_CreateMutex();
    worker->condition = SDL_CreateCondition();
    if (!worker->mutex || !worker->condition)
    {
        fprintf(stderr, "YOLO: mutex/condition impossible: %s\n", SDL_GetError());
        return 0;
    }

    SECURITY_ATTRIBUTES security_attributes;
    memset(&security_attributes, 0, sizeof(security_attributes));
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    HANDLE stdin_read = NULL;
    HANDLE stdout_write = NULL;

    if (!CreatePipe(&stdin_read, &worker->stdin_write, &security_attributes, 0) ||
        !CreatePipe(&worker->stdout_read, &stdout_write, &security_attributes, 0))
    {
        fprintf(stderr, "YOLO: creation des pipes impossible: %lu\n", GetLastError());
        if (stdin_read)
            CloseHandle(stdin_read);
        if (stdout_write)
            CloseHandle(stdout_write);
        return 0;
    }

    SetHandleInformation(worker->stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(worker->stdout_read, HANDLE_FLAG_INHERIT, 0);

    char command_line[512];
    snprintf(command_line, sizeof(command_line),
             "\"%s\" -u \"%s\" --model \"%s\" --imgsz %d --conf %.3f",
             YOLO_PYTHON_EXE, YOLO_WORKER_SCRIPT, YOLO_MODEL_PATH, YOLO_INPUT_SIZE, YOLO_CONFIDENCE);

    STARTUPINFOA startup_info;
    PROCESS_INFORMATION process_info;
    memset(&startup_info, 0, sizeof(startup_info));
    memset(&process_info, 0, sizeof(process_info));
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = stdin_read;
    startup_info.hStdOutput = stdout_write;
    startup_info.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    if (!CreateProcessA(NULL, command_line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup_info, &process_info))
    {
        fprintf(stderr, "YOLO: impossible de lancer le worker Python: %lu\n", GetLastError());
        CloseHandle(stdin_read);
        CloseHandle(stdout_write);
        CloseHandle(worker->stdin_write);
        CloseHandle(worker->stdout_read);
        worker->stdin_write = NULL;
        worker->stdout_read = NULL;
        return 0;
    }

    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    CloseHandle(process_info.hThread);
    worker->process = process_info.hProcess;

    worker->thread = SDL_CreateThread(yolo_worker_thread, "yolo-worker", worker);
    if (!worker->thread)
    {
        fprintf(stderr, "YOLO: impossible de creer le thread: %s\n", SDL_GetError());
        TerminateProcess(worker->process, 1);
        CloseHandle(worker->process);
        CloseHandle(worker->stdin_write);
        CloseHandle(worker->stdout_read);
        worker->process = NULL;
        worker->stdin_write = NULL;
        worker->stdout_read = NULL;
        return 0;
    }

    worker->enabled = 1;
    snprintf(worker->status, sizeof(worker->status), "YOLO: chargement de %s", YOLO_MODEL_PATH);
    return 1;
}

static void yolo_worker_stop(YoloWorker *worker)
{
    if (!worker)
    {
        return;
    }

    if (worker->mutex)
    {
        SDL_LockMutex(worker->mutex);
        worker->stop = 1;
        if (worker->condition)
        {
            SDL_SignalCondition(worker->condition);
        }
        SDL_UnlockMutex(worker->mutex);
    }

    if (worker->stdin_write)
    {
        CloseHandle(worker->stdin_write);
        worker->stdin_write = NULL;
    }

    if (worker->process)
    {
        TerminateProcess(worker->process, 0);
    }

    if (worker->thread)
    {
        SDL_WaitThread(worker->thread, NULL);
        worker->thread = NULL;
    }

    if (worker->stdout_read)
    {
        CloseHandle(worker->stdout_read);
        worker->stdout_read = NULL;
    }

    if (worker->process)
    {
        CloseHandle(worker->process);
        worker->process = NULL;
    }

    free(worker->pending_frame);
    worker->pending_frame = NULL;
    worker->pending_capacity = 0;

    if (worker->condition)
    {
        SDL_DestroyCondition(worker->condition);
        worker->condition = NULL;
    }

    if (worker->mutex)
    {
        SDL_DestroyMutex(worker->mutex);
        worker->mutex = NULL;
    }
}

static int yolo_submit_surface(YoloWorker *worker, SDL_Surface *surface)
{
    if (!worker || !worker->enabled || !surface)
    {
        return 0;
    }

    SDL_LockMutex(worker->mutex);
    if (worker->has_pending || worker->stop)
    {
        SDL_UnlockMutex(worker->mutex);
        return 0;
    }
    SDL_UnlockMutex(worker->mutex);

    SDL_Surface *rgb = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGB24);
    if (!rgb)
    {
        return 0;
    }

    const int row_bytes = rgb->w * 3;
    const size_t frame_size = (size_t)row_bytes * (size_t)rgb->h;
    int accepted = 0;

    SDL_LockMutex(worker->mutex);
    if (!worker->has_pending)
    {
        if (frame_size > worker->pending_capacity)
        {
            unsigned char *resized = (unsigned char *)realloc(worker->pending_frame, frame_size);
            if (resized)
            {
                worker->pending_frame = resized;
                worker->pending_capacity = frame_size;
            }
        }

        if (frame_size <= worker->pending_capacity)
        {
            const unsigned char *source = (const unsigned char *)rgb->pixels;
            for (int y = 0; y < rgb->h; ++y)
            {
                memcpy(worker->pending_frame + (size_t)y * (size_t)row_bytes,
                       source + (size_t)y * (size_t)rgb->pitch,
                       (size_t)row_bytes);
            }

            worker->pending_size = frame_size;
            worker->pending_width = rgb->w;
            worker->pending_height = rgb->h;
            worker->has_pending = 1;
            accepted = 1;
            SDL_SignalCondition(worker->condition);
        }
    }
    SDL_UnlockMutex(worker->mutex);

    SDL_DestroySurface(rgb);
    return accepted;
}

static void draw_yolo_detections(SDL_Renderer *renderer, YoloWorker *worker, int window_width, int window_height)
{
    if (!worker || !worker->enabled || window_width <= 0 || window_height <= 0)
    {
        return;
    }

    YoloDetection detections[YOLO_MAX_DETECTIONS];
    int detection_count = 0;
    int frame_width = 0;
    int frame_height = 0;

    SDL_LockMutex(worker->mutex);
    detection_count = worker->detection_count;
    frame_width = worker->frame_width;
    frame_height = worker->frame_height;
    memcpy(detections, worker->detections, sizeof(detections));
    SDL_UnlockMutex(worker->mutex);

    if (detection_count <= 0 || frame_width <= 0 || frame_height <= 0)
    {
        return;
    }

    const float scale_x = (float)window_width / (float)frame_width;
    const float scale_y = (float)window_height / (float)frame_height;

    for (int i = 0; i < detection_count; ++i)
    {
        SDL_FPoint points[5];
        for (int p = 0; p < 4; ++p)
        {
            points[p].x = detections[i].x[p] * scale_x;
            points[p].y = detections[i].y[p] * scale_y;
        }
        points[4] = points[0];

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        for (int offset = -1; offset <= 1; ++offset)
        {
            SDL_FPoint shadow[5];
            for (int p = 0; p < 5; ++p)
            {
                shadow[p].x = points[p].x + (float)offset;
                shadow[p].y = points[p].y + 1.0f;
            }
            SDL_RenderLines(renderer, shadow, 5);
        }

        SDL_SetRenderDrawColor(renderer, 0, 255, 80, 255);
        SDL_RenderLines(renderer, points, 5);
        SDL_RenderDebugTextFormat(renderer, points[0].x + 3.0f, points[0].y + 3.0f,
                                  "%d %.2f", detections[i].class_id, detections[i].confidence);
    }
}

static void yolo_copy_status(YoloWorker *worker, char *buffer, size_t capacity)
{
    if (!worker || !worker->mutex || capacity == 0)
    {
        if (capacity > 0)
        {
            buffer[0] = '\0';
        }
        return;
    }

    SDL_LockMutex(worker->mutex);
    snprintf(buffer, capacity, "%s", worker->status);
    SDL_UnlockMutex(worker->mutex);
}

static void yolo_get_snapshot(YoloWorker *worker, YoloSnapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));

    if (!worker || !worker->mutex)
    {
        return;
    }

    SDL_LockMutex(worker->mutex);
    snapshot->model_ready = worker->model_ready;
    snapshot->frame_width = worker->frame_width;
    snapshot->frame_height = worker->frame_height;
    snapshot->has_detection = worker->model_ready &&
                              worker->detection_count > 0 &&
                              worker->frame_width > 0 &&
                              worker->frame_height > 0;
    if (snapshot->has_detection)
    {
        snapshot->detection = worker->detections[0];
    }
    SDL_UnlockMutex(worker->mutex);
}
#else
typedef struct
{
    int unused;
} YoloWorker;

static int yolo_worker_start(YoloWorker *worker)
{
    (void)worker;
    return 0;
}

static void yolo_worker_stop(YoloWorker *worker)
{
    (void)worker;
}

static int yolo_submit_surface(YoloWorker *worker, SDL_Surface *surface)
{
    (void)worker;
    (void)surface;
    return 0;
}

static void draw_yolo_detections(SDL_Renderer *renderer, YoloWorker *worker, int window_width, int window_height)
{
    (void)renderer;
    (void)worker;
    (void)window_width;
    (void)window_height;
}

static void yolo_copy_status(YoloWorker *worker, char *buffer, size_t capacity)
{
    (void)worker;
    if (capacity > 0)
    {
        snprintf(buffer, capacity, "YOLO: indisponible hors Windows");
    }
}

static void yolo_get_snapshot(YoloWorker *worker, YoloSnapshot *snapshot)
{
    (void)worker;
    memset(snapshot, 0, sizeof(*snapshot));
}
#endif

static float normalize_axis(Sint16 value)
{
    const int deadzone = 8000;
    if (value > -deadzone && value < deadzone)
    {
        return 0.0f;
    }

    return (float)value / 32767.0f;
}

static int axis_to_percent(Sint16 value)
{
    float normalized = normalize_axis(value);
    if (normalized < -1.0f)
    {
        normalized = -1.0f;
    }
    if (normalized > 1.0f)
    {
        normalized = 1.0f;
    }

    return (int)(normalized * 100.0f);
}

static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value)
    {
        return min_value;
    }

    if (value > max_value)
    {
        return max_value;
    }

    return value;
}

static int axis_to_motor_delay_us(int percent)
{
    if (percent == 0)
    {
        return 0;
    }

    const int max_delay_us = 2000;
    const int min_delay_us = 200;
    int magnitude = percent < 0 ? -percent : percent;

    if (magnitude > 100)
    {
        magnitude = 100;
    }

    return max_delay_us - ((max_delay_us - min_delay_us) * magnitude) / 100;
}

static float clamp_float(float value, float min_value, float max_value)
{
    if (value < min_value)
    {
        return min_value;
    }

    if (value > max_value)
    {
        return max_value;
    }

    return value;
}

static void pid_init(PidController *pid, float kp, float ki, float kd)
{
    memset(pid, 0, sizeof(*pid));
    pid->kp = kp;
    pid->ki = ki;
    pid->kd = kd;
}

static void auto_aim_init(AutoAimState *state)
{
    pid_init(&state->x, AUTO_AIM_X_KP, AUTO_AIM_X_KI, AUTO_AIM_X_KD);
    pid_init(&state->y, AUTO_AIM_Y_KP, AUTO_AIM_Y_KI, AUTO_AIM_Y_KD);
    state->active_last_frame = 0;
}

static void pid_reset(PidController *pid)
{
    pid->integral = 0.0f;
    pid->previous_error = 0.0f;
    pid->has_previous_error = 0;
    pid->previous_ticks_ms = 0;
}

static void auto_aim_reset(AutoAimState *state)
{
    pid_reset(&state->x);
    pid_reset(&state->y);
    state->active_last_frame = 0;
}

static float pid_update(PidController *pid, float error, Uint64 now_ms)
{
    float dt = 0.016f;
    float derivative = 0.0f;

    if (pid->previous_ticks_ms > 0 && now_ms > pid->previous_ticks_ms)
    {
        dt = (float)(now_ms - pid->previous_ticks_ms) / 1000.0f;
        dt = clamp_float(dt, 0.001f, 0.100f);
    }

    pid->integral += error * dt;
    pid->integral = clamp_float(pid->integral, -1.0f, 1.0f);

    if (pid->has_previous_error)
    {
        derivative = (error - pid->previous_error) / dt;
    }

    pid->previous_error = error;
    pid->previous_ticks_ms = now_ms;
    pid->has_previous_error = 1;

    return pid->kp * error + pid->ki * pid->integral + pid->kd * derivative;
}

static void detection_center(const YoloDetection *detection, float *center_x, float *center_y)
{
    *center_x = 0.0f;
    *center_y = 0.0f;

    for (int i = 0; i < 4; ++i)
    {
        *center_x += detection->x[i];
        *center_y += detection->y[i];
    }

    *center_x *= 0.25f;
    *center_y *= 0.25f;
}

static float cross_product_2d(float ax, float ay, float bx, float by, float cx, float cy)
{
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static int point_in_quad(float px, float py, const float x[4], const float y[4])
{
    int has_negative = 0;
    int has_positive = 0;

    for (int i = 0; i < 4; ++i)
    {
        int next = (i + 1) % 4;
        float cross = cross_product_2d(x[i], y[i], x[next], y[next], px, py);

        if (cross < 0.0f)
        {
            has_negative = 1;
        }
        else if (cross > 0.0f)
        {
            has_positive = 1;
        }
    }

    return !(has_negative && has_positive);
}

static int aim_point_in_detection_center_zone(const YoloDetection *detection, float aim_x, float aim_y)
{
    float center_x = 0.0f;
    float center_y = 0.0f;
    float zone_x[4];
    float zone_y[4];

    detection_center(detection, &center_x, &center_y);

    for (int i = 0; i < 4; ++i)
    {
        zone_x[i] = center_x + (detection->x[i] - center_x) * AUTO_AIM_CENTER_ZONE_RATIO;
        zone_y[i] = center_y + (detection->y[i] - center_y) * AUTO_AIM_CENTER_ZONE_RATIO;
    }

    return point_in_quad(aim_x, aim_y, zone_x, zone_y);
}

static int auto_aim_compute(AutoAimState *state, const YoloSnapshot *snapshot, int *turn_percent, int *throttle_percent, int *is_aligned)
{
    if (!AUTO_AIM_ENABLED || !snapshot->has_detection || snapshot->frame_width <= 0 || snapshot->frame_height <= 0)
    {
        auto_aim_reset(state);
        *is_aligned = 0;
        return 0;
    }

    float target_x = 0.0f;
    float target_y = 0.0f;
    float aim_x = (float)snapshot->frame_width * AIM_POINT_X_RATIO;
    float aim_y = (float)snapshot->frame_height * AIM_POINT_Y_RATIO;

    detection_center(&snapshot->detection, &target_x, &target_y);

    *is_aligned = aim_point_in_detection_center_zone(&snapshot->detection, aim_x, aim_y);
    if (*is_aligned)
    {
        auto_aim_reset(state);
        *turn_percent = 0;
        *throttle_percent = 0;
        state->active_last_frame = 1;
        return 1;
    }

    float error_x = (target_x - aim_x) / ((float)snapshot->frame_width * 0.5f);
    float error_y = (target_y - aim_y) / ((float)snapshot->frame_height * 0.5f);

    if (AUTO_AIM_INVERT_X)
    {
        error_x = -error_x;
    }
    if (AUTO_AIM_INVERT_Y)
    {
        error_y = -error_y;
    }

    error_x = clamp_float(error_x, -1.0f, 1.0f);
    error_y = clamp_float(error_y, -1.0f, 1.0f);

    const Uint64 now_ms = SDL_GetTicks();
    float turn_output = pid_update(&state->x, error_x, now_ms);
    float throttle_output = pid_update(&state->y, error_y, now_ms);

    turn_output = clamp_float(turn_output, -AUTO_AIM_MAX_OUTPUT_PERCENT, AUTO_AIM_MAX_OUTPUT_PERCENT);
    throttle_output = clamp_float(throttle_output, -AUTO_AIM_MAX_OUTPUT_PERCENT, AUTO_AIM_MAX_OUTPUT_PERCENT);

    *turn_percent = (int)turn_output;
    *throttle_percent = (int)throttle_output;
    state->active_last_frame = 1;
    return 1;
}

static int camera_spec_score(const SDL_CameraSpec *spec)
{
    int fps = 0;
    int fps_penalty = CAMERA_TARGET_FPS * 10;

    if (!spec)
    {
        return 0x7fffffff;
    }

    if (spec->framerate_denominator > 0)
    {
        fps = spec->framerate_numerator / spec->framerate_denominator;
        fps_penalty = abs(fps - CAMERA_TARGET_FPS) * 10;
    }

    return abs(spec->width - CAMERA_TARGET_WIDTH) * 3 +
           abs(spec->height - CAMERA_TARGET_HEIGHT) * 3 +
           fps_penalty;
}

static SDL_Camera *open_camera_low_latency(SDL_CameraID camera_id, int camera_index)
{
    int format_count = 0;
    SDL_CameraSpec **formats = SDL_GetCameraSupportedFormats(camera_id, &format_count);
    SDL_CameraSpec desired;
    SDL_CameraSpec *desired_ptr = NULL;

    memset(&desired, 0, sizeof(desired));

    if (formats && format_count > 0)
    {
        int best_index = 0;
        int best_score = camera_spec_score(formats[0]);

        for (int i = 1; i < format_count; ++i)
        {
            int score = camera_spec_score(formats[i]);
            if (score < best_score)
            {
                best_score = score;
                best_index = i;
            }
        }

        desired = *formats[best_index];
        desired_ptr = &desired;
        fprintf(stderr, "Camera %d: format demande %dx%d @ %d/%d fps\n",
                camera_index,
                desired.width,
                desired.height,
                desired.framerate_numerator,
                desired.framerate_denominator);
    }

    SDL_Camera *camera = SDL_OpenCamera(camera_id, desired_ptr);
    SDL_free(formats);
    return camera;
}

#ifdef _WIN32
typedef struct
{
    HANDLE handle;
} SerialPort;

static int serial_open(SerialPort *port, const char *port_name, DWORD baud_rate)
{
    DCB dcb;
    COMMTIMEOUTS timeouts;

    port->handle = CreateFileA(port_name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (port->handle == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(port->handle, &dcb))
    {
        CloseHandle(port->handle);
        port->handle = INVALID_HANDLE_VALUE;
        return 0;
    }

    dcb.BaudRate = baud_rate;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;

    if (!SetCommState(port->handle, &dcb))
    {
        CloseHandle(port->handle);
        port->handle = INVALID_HANDLE_VALUE;
        return 0;
    }

    memset(&timeouts, 0, sizeof(timeouts));
    timeouts.WriteTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    SetCommTimeouts(port->handle, &timeouts);
    SetupComm(port->handle, 4096, 4096);
    PurgeComm(port->handle, PURGE_TXCLEAR | PURGE_TXABORT);

    return 1;
}

static void serial_close(SerialPort *port)
{
    if (port->handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(port->handle);
        port->handle = INVALID_HANDLE_VALUE;
    }
}

static int serial_write_line(SerialPort *port, const char *line)
{
    DWORD written = 0;
    size_t length = strlen(line);

    if (port->handle == INVALID_HANDLE_VALUE)
    {
        return 0;
    }

    return WriteFile(port->handle, line, (DWORD)length, &written, NULL) && written == length;
}
#endif

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_CAMERA | SDL_INIT_GAMEPAD))
    {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("S.A.D.O Controller", 800, 600, SDL_WINDOW_RESIZABLE);
    if (!window)
    {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer)
    {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    int camera_count = 0;
    SDL_CameraID *cameras = SDL_GetCameras(&camera_count);

    if (!cameras || camera_count <= 0)
    {
        fprintf(stderr, "Aucune webcam detectee: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    int cam_index = 0;
    SDL_Camera *camera = open_camera_low_latency(cameras[cam_index], cam_index);
    if (!camera)
    {
        fprintf(stderr, "SDL_OpenCamera failed: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_Gamepad *gamepad = NULL;
    int gamepad_count = 0;
    SDL_JoystickID *gamepads = SDL_GetGamepads(&gamepad_count);
    if (gamepads && gamepad_count > 0)
    {
        gamepad = SDL_OpenGamepad(gamepads[0]);
    }
    SDL_free(gamepads);

    if (!gamepad)
    {
        fprintf(stderr, "Aucune manette detectee ou ouverture impossible: %s\n", SDL_GetError());
    }

#ifdef _WIN32
    SerialPort serial_port = {INVALID_HANDLE_VALUE};
    int serial_ready = serial_open(&serial_port, "\\\\.\\COM6", CBR_115200);
    char last_command[64] = "";

    if (!serial_ready)
    {
        fprintf(stderr, "Impossible d'ouvrir COM6: %lu\n", GetLastError());
    }
#else
    int serial_ready = 0;
#endif

    YoloWorker yolo_worker;
    yolo_worker_start(&yolo_worker);
    AutoAimState auto_aim;
    auto_aim_init(&auto_aim);

    SDL_Texture *camera_texture = NULL;
    SDL_Event event;
    int running = 1;
    int rt_was_pressed = 0;
    Uint64 last_yolo_submit_ms = 0;

    Uint64 last_fire_ms = 0;
    int auto_aim_active = 0;
    int auto_aim_aligned = 0;
    int auto_turn_percent = 0;
    int auto_throttle_percent = 0;

    while (running)
    {
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT)
            {
                running = 0;
            }
        }

        if (gamepad && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH))
        {
            // change camera
            cam_index = (cam_index + 1) % camera_count;
            SDL_CloseCamera(camera);
            camera = open_camera_low_latency(cameras[cam_index], cam_index);
            if (!camera)
            {
                fprintf(stderr, "ERROR WHILE OPENING CAMERA %d: %s\n", cam_index, SDL_GetError());
            }
            else
            {
                fprintf(stderr, "Switched to camera %d\n", cam_index);
            }
            SDL_Delay(10);
        }

        SDL_UpdateJoysticks();

        if (!gamepad)
        {
            int refreshed_count = 0;
            SDL_JoystickID *refreshed_gamepads = SDL_GetGamepads(&refreshed_count);
            if (refreshed_gamepads && refreshed_count > 0)
            {
                gamepad = SDL_OpenGamepad(refreshed_gamepads[0]);
            }
            SDL_free(refreshed_gamepads);
        }

        Uint64 timestamp = 0;
        Uint64 latest_timestamp = 0;
        SDL_Surface *latest_frame = NULL;
        SDL_Surface *frame = NULL;

        while (camera && (frame = SDL_AcquireCameraFrame(camera, &timestamp)) != NULL)
        {
            if (latest_frame)
            {
                SDL_ReleaseCameraFrame(camera, latest_frame);
            }

            latest_frame = frame;
            latest_timestamp = timestamp;
        }

        if (latest_frame)
        {
            SDL_Texture *new_texture = SDL_CreateTextureFromSurface(renderer, latest_frame);

            const Uint64 now_ms = SDL_GetTicks();
            if (now_ms - last_yolo_submit_ms >= YOLO_FRAME_INTERVAL_MS)
            {
                if (yolo_submit_surface(&yolo_worker, latest_frame))
                {
                    last_yolo_submit_ms = now_ms;
                }
            }

            SDL_ReleaseCameraFrame(camera, latest_frame);
            (void)latest_timestamp;

            if (new_texture)
            {
                if (camera_texture)
                {
                    SDL_DestroyTexture(camera_texture);
                }

                camera_texture = new_texture;
            }
        }

        Sint16 left_x_raw = 0;
        Sint16 left_y_raw = 0;
        Sint16 rt_raw = 0;

        int fire = false;

        if (gamepad)
        {
            left_x_raw = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
            left_y_raw = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
            rt_raw = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

            const int rt_pressed = rt_raw > 12000;
            if (rt_pressed && !rt_was_pressed)
            {
                SDL_RumbleGamepad(gamepad, 0xFFFF, 0xFFFF, 100);
                SDL_RumbleGamepadTriggers(gamepad, 0x3000, 0xC000, 120);
            }
            rt_was_pressed = rt_pressed;
            Uint64 now_ms = SDL_GetTicks();
            if (rt_pressed && now_ms - last_fire_ms >= 800)
            {
                fire = true;
            }
        }
        else
        {
            rt_was_pressed = 0;
        }

        YoloSnapshot yolo_snapshot;
        yolo_get_snapshot(&yolo_worker, &yolo_snapshot);
        auto_turn_percent = 0;
        auto_throttle_percent = 0;
        auto_aim_aligned = 0;
        auto_aim_active = auto_aim_compute(&auto_aim,
                                           &yolo_snapshot,
                                           &auto_turn_percent,
                                           &auto_throttle_percent,
                                           &auto_aim_aligned);

        if (serial_ready && (gamepad || auto_aim_active))
        {
            int throttle = axis_to_percent(left_y_raw);
            int turn = axis_to_percent(left_x_raw);

            if (auto_aim_active)
            {
                turn = auto_turn_percent;
                throttle = auto_throttle_percent;
            }

            int left_motor = auto_aim_active ? clamp_int(turn, -100, 100) : clamp_int((int)(turn * 0.5f), -100, 100);
            int right_motor = clamp_int(throttle, -100, 100);
            int dir1 = left_motor >= 0 ? 1 : 0;
            int dir2 = right_motor >= 0 ? 1 : 0;
            int delay1 = axis_to_motor_delay_us(left_motor);
            int delay2 = axis_to_motor_delay_us(right_motor);
            char command[64];

            if (fire)
            {
                last_fire_ms = SDL_GetTicks();
                printf("FIRE!");
            }

            snprintf(command, sizeof(command), "%d,%d,%d,%d,%d\n", dir1, delay1, dir2, delay2, fire ? 1 : 0);

            if (strcmp(command, last_command) != 0)
            {
                printf("Sending command: %s", command);

                if (!serial_write_line(&serial_port, command))
                {
                    fprintf(stderr, "Ecriture serie echouee sur COM6\n");
                    serial_ready = 0;
                }
                else
                {
                    strcpy(last_command, command);
                }
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        if (camera_texture)
        {
            SDL_RenderTexture(renderer, camera_texture, NULL, NULL);
        }

        int window_width = 0;
        int window_height = 0;
        SDL_GetWindowSize(window, &window_width, &window_height);

        draw_yolo_detections(renderer, &yolo_worker, window_width, window_height);

        char yolo_status[160];
        yolo_copy_status(&yolo_worker, yolo_status, sizeof(yolo_status));
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderDebugTextFormat(renderer, 9.0f, 9.0f, "%s", yolo_status);
        SDL_SetRenderDrawColor(renderer, 180, 255, 180, 255);
        SDL_RenderDebugTextFormat(renderer, 8.0f, 8.0f, "%s", yolo_status);

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderDebugTextFormat(renderer, 9.0f, 21.0f,
                                  "AUTO: %s  X:%4d%%  Y:%4d%%",
                                  auto_aim_active ? (auto_aim_aligned ? "LOCK" : "PID") : (yolo_snapshot.model_ready ? "NO TARGET" : "LOADING"),
                                  auto_turn_percent,
                                  auto_throttle_percent);
        SDL_SetRenderDrawColor(renderer, 255, 230, 120, 255);
        SDL_RenderDebugTextFormat(renderer, 8.0f, 20.0f,
                                  "AUTO: %s  X:%4d%%  Y:%4d%%",
                                  auto_aim_active ? (auto_aim_aligned ? "LOCK" : "PID") : (yolo_snapshot.model_ready ? "NO TARGET" : "LOADING"),
                                  auto_turn_percent,
                                  auto_throttle_percent);

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderDebugTextFormat(renderer, 9.0f, (float)window_height - 31.0f,
                                  "LX: %4d%%  LY: %4d%%", axis_to_percent(left_x_raw), -axis_to_percent(left_y_raw));
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDebugTextFormat(renderer, 8.0f, (float)window_height - 32.0f,
                                  "LX: %4d%%  LY: %4d%%", axis_to_percent(left_x_raw), -axis_to_percent(left_y_raw));

        if (gamepad)
        {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderDebugTextFormat(renderer, 9.0f, (float)window_height - 19.0f,
                                      "RT: %4d%%  FIRE: %s", axis_to_percent(rt_raw), (rt_raw > 12000) ? "ON" : "OFF");
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDebugTextFormat(renderer, 8.0f, (float)window_height - 20.0f,
                                      "RT: %4d%%  FIRE: %s", axis_to_percent(rt_raw), (rt_raw > 12000) ? "ON" : "OFF");
        }
        else
        {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderDebugTextFormat(renderer, 9.0f, (float)window_height - 19.0f,
                                      "RT: --  FIRE: NO PAD");
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderDebugTextFormat(renderer, 8.0f, (float)window_height - 20.0f,
                                      "RT: --  FIRE: NO PAD");
        }

        SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);

        SDL_GetWindowSize(window, &window_width, &window_height);
        int rect_center_x = window_width / 1.82;
        int rect_center_y = window_height / 2.4;
        int rect_size = 20;
        SDL_RenderRect(renderer, &(const SDL_FRect){
                                     .x = (float)(rect_center_x - rect_size / 2),
                                     .y = (float)(rect_center_y - rect_size / 2),
                                     .w = (float)rect_size,
                                     .h = (float)rect_size,
                                 });
        SDL_RenderPresent(renderer);
        SDL_Delay(1);
    }

    if (camera_texture)
    {
        SDL_DestroyTexture(camera_texture);
    }

    if (gamepad)
    {
        SDL_CloseGamepad(gamepad);
    }

    yolo_worker_stop(&yolo_worker);

#ifdef _WIN32
    serial_close(&serial_port);
#endif

    SDL_CloseCamera(camera);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
