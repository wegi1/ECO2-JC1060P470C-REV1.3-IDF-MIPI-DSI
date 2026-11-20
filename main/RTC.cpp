//=====================================================================================================
#include "rtc.hpp"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <esp_timer.h>
#include <sys/stat.h>
#include <math.h>

// Naglowki FreeRTOS oraz ESP-IDF
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "driver/ledc.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_spiffs.h"
#include "esp_jpeg_dec.h"

// Naglowki Twojego podsystemu graficznego i sprzetu
#include "renderer.hpp"
#include "fonty.h"
#include "jd9165_lcd.h"
#include "gt911_touch.h"
#include "pins_config.h"
#include "rx8025t.h"
#include "wifi_ntp.hpp" // Potrzebne do synchronizacji NTP
#include "wifi_portal.hpp"
#include "vectordotsball.hpp"

//=====================================================================================================
// Definicje dla kanału PWM
#define BACKLIGHT_GPIO 23
#define BACKLIGHT_LEDC_TIMER LEDC_TIMER_0
#define BACKLIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
#define BACKLIGHT_LEDC_DUTY_RES LEDC_TIMER_10_BIT // Rozdzielczość 10-bitowa (0 - 1023)
#define BACKLIGHT_LEDC_FREQUENCY 5000             // Częstotliwość 5 kHz
//=====================================================================================================
// Deklaracje obiektow i buforow zdefiniowanych w main.cpp
extern jd9165_lcd lcd;

// ZAMYKAMY DEKLARACJĘ DOTYKU W BLOKU ZGODNOŚCI "C":
extern gt911_touch touch;

extern uint16_t *fbs[2]; // Bufory ramki (framebuffer) dla wyświetlacza
extern uint8_t *jpg_scratch_buf;
uint16_t *back_buffer = NULL;
extern SemaphoreHandle_t vsync_sem;
extern SemaphoreHandle_t i2c_mutex;
extern volatile uint8_t active_fb_idx;
// Wskaźnik (uchwyt) do zadania gaszenia dwukropka
TaskHandle_t hTaskDisableColon = NULL;
TaskHandle_t hTaskTouch = NULL; // Uchwyt do zadania dotyku
TaskHandle_t hTaskMenuEngine = NULL;
TaskHandle_t hTaskHardwareTouchDriver = NULL;

static const size_t BUFFER_SIZE = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);

// Zmienne statyczne do obsługi czasu POSIX
static struct tm dane_czasu;
static time_t sekundy_epoch;

// Bufory tekstowe na sformatowane cyfry
static char txt_godz[4];
static char txt_min[4];
static char txt_sek[4];

volatile bool ntp_read = false;
volatile bool temp_read = false;

volatile float g_outdoor_temp = -999.0f;

volatile bool is_editing_time = false;

volatile uint8_t BRIGHTNESS = 50; // Zakres 0 - 100
volatile uint8_t BRIGHTNESS_NIGHT = 20;
volatile uint32_t g_my_4h_counter = 0;
volatile bool g_trigger_auto_photo = false;
volatile int PHOTO_NR = 0;    // Zmienna globalna do przechowywania numeru aktualnego zdjęcia (1 lub 2)
static int old_seconds = 111; // Zmienna do filtrowania zbocza sekundy (Edge Detection)

SemaphoreHandle_t fb_mutex = NULL;
volatile uint16_t global_touchX = 0;
volatile uint16_t global_touchY = 0;
volatile bool global_is_touched = false;
volatile bool demo_exit_requested = false;

bool g_slide_enabled = true;
uint8_t g_slide_interval = 5;
uint8_t g_slide_photo_count = 2;
static const uint8_t SLIDE_INTERVALS[] = {5, 10, 15, 20, 30, 60};
static const int SLIDE_INTERVALS_COUNT = 6;

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// ==========================================
// CZĘŚĆ 2 Z 3: ZADANIE FREE_RTOS I INICJALIZACJA
// ==========================================
void vTaskDisableColon(void *pvParameters)
{
    // uint64_t rtc_start = 0;
    // uint64_t rtc_end = 0;
    // 3. Definiujemy bufory w sposób absolutny i bezpieczny:

    while (1)
    {
        // 1. Czekamy na sygnał wybudzenia z funkcji print_RTC()
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (is_editing_time)
        {
            continue;
        } // Jeśli edytujemy , nie ruszaj bufora zegara!
        // 2. Odliczamy dokładnie 500 milisekund (dwukropek świeci)
        vTaskDelay(pdMS_TO_TICKS(500));
        // Drugie sprawdzenie flagi is_editing_time po upływie 500ms
        if (is_editing_time)
        {
            continue;
        }

        // rtc_start = esp_timer_get_time();
        xSemaphoreTake(fb_mutex, portMAX_DELAY);
        // 3. Kopiujemy aktualnie widoczny ekran do wolnego back_buffer (2 MB)
        memcpy(back_buffer, (void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2);

        // 4. Od razu na nim GASIMY kwadraciki dwukropka
        draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 490, 80, 40, 40, 0);
        draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 490, 235, 40, 40, 0);
        flush_screen_vsync();
        xSemaphoreGive(fb_mutex);
        // rtc_end = esp_timer_get_time();
        // printf("[PROFILER] RENDER COLON (vTaskDisableColon) zajął: %llu us\n", (rtc_end - rtc_start));
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void vTaskHardwareTouchDriver(void *pvParameters)
{
    printf("[DRIVER] Niskopoziomowy wątek sprzętowy I2C (vTaskHardwareTouchDriver) uruchomiony.\n");

    int miss_count = 0;
    // Usypiamy go na starcie - obudzą go dopiero procedury menu jasności lub NTP
    vTaskSuspend(NULL);
    while (1)
    {
        uint16_t tx = 0;
        uint16_t ty = 0;
        bool touched = false;
        if (i2c_mutex != NULL && xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
        {
            touched = touch.getTouch(&tx, &ty);
            xSemaphoreGive(i2c_mutex);
        }

        if (touched)
        {
            miss_count = 0;
            global_touchX = tx;
            global_touchY = ty;
            global_is_touched = true;
            demo_exit_requested = true;
        }
        else if (++miss_count >= 3)
        {
            global_touchX = 0;
            global_touchY = 0;
            global_is_touched = false;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void vTaskTouch(void *pvParameters)
{
    uint16_t touchX = 0;
    uint16_t touchY = 0;

    printf("[LOG] Wielowątkowe zadanie dotyku vTaskTouch ruszyło z prawidłowym mapowaniem rotacji!\n");

    while (1)
    {
        if (is_editing_time)
        {
            vTaskDelay(pdMS_TO_TICKS(50)); // Śpij i oddaj procesor innym zadaniom
            continue;                      // Przeskocz resztę pętli, nie sprawdzaj ekranu głównego
        }
        bool touched = touch.getTouch(&touchX, &touchY);

        if (touched)
        {
            printf("[TOUCH] Odczyt fizyczny: X = %d, Y = %d\n", touchX, touchY);

            // 1. HIT-BOX: SEKCJA GODZIN (Góra-Lewo)
            if (touchY >= 1 && touchY <= 370)
            {
                if (touchX >= 1 && touchX <= 460)
                {
                    printf("[MENU] -> Wykryto kliknięcie w SEKCJĘ GODZIN!\n");
                    if (hTaskMenuEngine != NULL)
                    {
                        is_editing_time = true;
                        // Dla godzin wysyłamy wartość 1
                        xTaskNotify(hTaskMenuEngine, 2, eSetValueWithOverwrite); // Budzimy silnik menu
                        vTaskSuspend(NULL);                                      // <<< PEWNE SAMOZAWIESZENIE WĄTKU DOTYKU!
                    }
                }
                // 2. HIT-BOX: SEKCJA MINUT (Góra-Prawo) - Łączymy elegancko przez else if na osi Y!
                else if (touchX >= 555 && touchX <= 1023)
                {
                    printf("[MENU] -> vTaskTouch budzi MenuEngine i sam się zawiesza...\n");
                    if (hTaskMenuEngine != NULL)
                    {
                        is_editing_time = true;
                        // Dla godzin wysyłamy wartość 1
                        xTaskNotify(hTaskMenuEngine, 1, eSetValueWithOverwrite); // Budzimy silnik menu
                        vTaskSuspend(NULL);                                      // <<< PEWNE SAMOZAWIESZENIE WĄTKU DOTYKU!
                    }
                }
            }

            // 5. HIT-BOX: WEJŚCIE DO MENU JASNOŚCI (Osobny, czysty warunek)
            if (touchX >= 685 && touchX <= 1023 && touchY >= 490 && touchY <= 599)
            {
                printf("[TOUCH] -> Wykryto kliknięcie w strefę regulacji jasności!\n");
                if (hTaskMenuEngine != NULL)
                {
                    is_editing_time = true;
                    xTaskNotify(hTaskMenuEngine, 3, eSetValueWithOverwrite); // Wybudzamy silnik wartością 3
                    vTaskSuspend(NULL);                                      // Samozawieszenie wątku dotyku
                }
                old_seconds = 111;
            }

            // 4. HIT-BOX: BŁYSKAWICZNE ZEROWANIE SEKUND (Dodatkowy przycisk na ekranie głównym)
            if (touchX >= 1 && touchX <= 200 && touchY >= 375 && touchY <= 480)
            {
                printf("[TOUCH] -> Wyzwalanie zerowania sekund z ekranu głównego!\n");

                // Wywołujemy funkcję zerującą rejestr sprzętowy i synchronizującą ESP32P4
                reset_seconds_to_zero();

                // Wymuszamy natychmiastowe przerysowanie głównego zegara w kolejnym obiegu
                old_seconds = 111;

                // Anty-rebound (Debouncing) dla przycisku resetu
                vTaskDelay(pdMS_TO_TICKS(250));
            }
            // SEKCJA ROKU
            if (touchX >= 1 && touchX <= 250 && touchY >= 485 && touchY <= 599)
            {
                printf("[TOUCH] -> Wykryto kliknięcie w USTAWIENIE ROKU!\n");
                if (hTaskMenuEngine != NULL)
                {
                    is_editing_time = true;
                    xTaskNotify(hTaskMenuEngine, 6, eSetValueWithOverwrite); // Wybudzamy silnik wartością 3
                    vTaskSuspend(NULL);                                      // Samozawieszenie wątku dotyku
                }
                old_seconds = 111;
            }
            // SEKCJA MIESIĘCY
            if (touchX >= 260 && touchX <= 512 && touchY >= 485 && touchY <= 599)
            {
                printf("[TOUCH] -> Wykryto kliknięcie w USTAWIENIE MIESIĘCY!\n");
                if (hTaskMenuEngine != NULL)
                {
                    is_editing_time = true;
                    xTaskNotify(hTaskMenuEngine, 5, eSetValueWithOverwrite); // Wybudzamy silnik wartością 3
                    vTaskSuspend(NULL);                                      // Samozawieszenie wątku dotyku
                }
                old_seconds = 111;
            }
            // SEKCJA DNI
            if (touchX >= 520 && touchX <= 680 && touchY >= 485 && touchY <= 599)
            {
                printf("[TOUCH] -> Wykryto kliknięcie w USTAWIENIE DNI!\n");
                if (hTaskMenuEngine != NULL)
                {
                    is_editing_time = true;
                    xTaskNotify(hTaskMenuEngine, 4, eSetValueWithOverwrite); // Wybudzamy silnik wartością 3
                    vTaskSuspend(NULL);                                      // Samozawieszenie wątku dotyku
                }
                old_seconds = 111;
            }
            // --- NTP --- HIT-BOX: WEJŚCIE DO MENU RĘCZNEJ SYNCHRONIZACJI NTP (Pasek stanu na dole) ---
            if (touchX >= 220 && touchX <= 1023 && touchY >= 375 && touchY <= 480)
            {
                printf("[TOUCH] -> Kliknięcie w pasek stanu! Uruchamiam Menu NTP...\n");
                if (hTaskMenuEngine != NULL)
                {
                    is_editing_time = true;
                    xTaskNotify(hTaskMenuEngine, 7, eSetValueWithOverwrite); // Wybudzamy MenuEngine wartością 7
                    vTaskSuspend(NULL);                                      // Samozawieszenie wątku dotyku
                }
                old_seconds = 111;
            }

            // Sprzętowy anty-rebound (Debouncing)
            vTaskDelay(pdMS_TO_TICKS(250));
        }

        // synchronixacja RTC z RX8025T co 4 godziny (g_my_4h_counter jest inkrementowany w vTaskFourHourIncrementer)
        if (g_my_4h_counter != 0)
        {
            sync_internal_rtc_with_rx8025t();

            old_seconds = 111;
            g_my_4h_counter = 0;
            printf("[RESYNC] -> Zsynchronizowano RTC z RX8025T...\n");
        }

        // Wyzwalanie pokazu zdjęcia co 5 minut (g_trigger_auto_photo jest ustawiany w vTaskAutoPhotoTimer)
        if (g_trigger_auto_photo == true)
        {
            if (!g_slide_enabled)
            {
                g_trigger_auto_photo = false;
                continue;
            }
            printf("[TOUCH] Przechwycono sygnal auto-foto! Blokuje czas i skacze do wyswietlarki...\n");
            is_editing_time = true;
            time_t now_photo = time(NULL);
            struct tm tm_photo;
            localtime_r(&now_photo, &tm_photo);
            uint8_t target_br = (tm_photo.tm_hour >= 20 || tm_photo.tm_hour < 6) ? BRIGHTNESS_NIGHT : BRIGHTNESS;

            uint16_t tx = 0, ty = 0;
            bool aborted = false;

            char photo_path[32];
            int photo_idx = (PHOTO_NR % g_slide_photo_count) + 1;
            snprintf(photo_path, sizeof(photo_path), "/spiffs/FOTO%d.jpg", photo_idx);
            struct stat st_photo;
            if (stat(photo_path, &st_photo) != 0)
            {
                PHOTO_NR = 0;
                snprintf(photo_path, sizeof(photo_path), "/spiffs/FOTO1.jpg");
            }
            wyswietl_jpeg_ze_spiffs(photo_path);
            PHOTO_NR = (PHOTO_NR + 1) % g_slide_photo_count;

            for (int i = 0; i < 480; i++)
            {
                if (touch.getTouch(&tx, &ty))
                {
                    aborted = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(25));
            }
            while (touch.getTouch(&tx, &ty))
                vTaskDelay(pdMS_TO_TICKS(20));
            for (int t = target_br; t >= 0; t--)
            {
                if (!aborted && touch.getTouch(&tx, &ty))
                    aborted = true;
                ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, (t * 1023) / 100);
                ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
                vTaskDelay(pdMS_TO_TICKS(aborted ? 5 : 40));
            }
            xSemaphoreTake(fb_mutex, portMAX_DELAY);
            safe_clear_screen(back_buffer, 0x0000);
            flush_screen_vsync();
            xSemaphoreGive(fb_mutex);
            old_seconds = 111;
            PHOTO_NR = PHOTO_NR ^ 1;
            g_trigger_auto_photo = false;
            while (touch.getTouch(&tx, &ty))
                vTaskDelay(pdMS_TO_TICKS(20));
            vTaskDelay(pdMS_TO_TICKS(150));
            touch.getTouch(&tx, &ty);
            is_editing_time = false;

            for (int t = 0; t <= target_br; t++)
            {
                ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, (t * 1023) / 100);
                ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
                vTaskDelay(pdMS_TO_TICKS(25));
            }
            apply_scheduled_brightness(tm_photo.tm_hour, true);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void vTaskWeather(void *pvParameters)
{
    printf("[WEATHER] Zadanie pogodowe wystartowalo na rdzeniu %d.\n", xPortGetCoreID());

    // Dajemy 3 sekundy na start zadania NTP, a potem grzecznie czekamy, az NTP skonczy!
    vTaskDelay(pdMS_TO_TICKS(3000));
    while (is_ntp_sync_running())
    {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    vTaskDelay(pdMS_TO_TICKS(1000)); // 1s oddechu po zamknieciu Wi-Fi przez NTP

    while (1)
    {
        if (!is_wifi_portal_active() && !is_ntp_sync_running())
        {
            float temp_val = -999.0f;
            if (fetch_weather_temperature(&temp_val))
            {
                g_outdoor_temp = temp_val;
                old_seconds = 111; // Wymuszenie odswiezenia ekranu
                printf("[WEATHER] Zaktualizowano temperature: %.1f C\n", g_outdoor_temp);
            }
            else
            {
                printf("[WEATHER] Nie udalo sie pobrac temperatury (zachowuje poprzednia).\n");
            }
        }
        else
        {
            printf("[WEATHER] Radio zajete przez NTP lub SoftAP - pomijam cykl.\n");
        }

        // Odswiezanie co 10 minut
        vTaskDelay(pdMS_TO_TICKS(10 * 60 * 1000));
    }
}

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// Funkcja wywoływana raz w app_main() w celu rejestracji zadań w jądrze FreeRTOS
void init_RTC_subsystem(void)
{

    char test_ssid[33] = {0};
    char test_pass[65] = {0};
    // Tworzenie wątku na rdzeniu 1 (peryferyjnym)
    xTaskCreatePinnedToCore(vTaskHardwareTouchDriver, "vTaskHwTouch", 8192, NULL, 3, &hTaskHardwareTouchDriver, 1);

    // NOWOŚĆ: Montujemy dysk zanim ruszą zadania FreeRTOS
    // tu jest plik z danymi dla WiFi
    init_spiffs_storage();
    // save_wifi_credentials_to_SPIFFS("SSID", "Password"); // Zapisz dane Wi-Fi do SPIFFS (tylko raz, jeśli nie zapisano wcześniej)
    if (load_wifi_credentials_from_SPIFFS(test_ssid, sizeof(test_ssid), test_pass, sizeof(test_pass)))
    {
        printf("[SPIFFS_TEST] SUKCES! Odczytano z SPIFFS -> SSID oraz HASLO");
    }
    else
    {
        printf("[SPIFFS_TEST] BLAD! Brak danych w SPIFFS !\n");
    }

    fb_mutex = xSemaphoreCreateMutex();

    // TESTOWE WYŚWIETLENIE PIERWSZEGO ZDJĘCIA (Zaraz po starcie na chwilę pokaże FOTO1.jpg!)
    // Wygaszamy ekran na start, zanim pokażemy zdjęcie
    load_slide_settings_from_NVS();
    ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, 0);
    ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);

    if (g_slide_enabled)
    {
        uint16_t tx = 0, ty = 0;
        bool aborted = false;
        wyswietl_jpeg_ze_spiffs("/spiffs/FOTO1.jpg");

        for (int t = 0; t <= 100; t++)
        {
            ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, (t * 1023) / 100);
            ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
            if (touch.getTouch(&tx, &ty))
            {
                aborted = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(15));
        }
        for (int i = 0; !aborted && i < 200; i++)
        {
            if (touch.getTouch(&tx, &ty))
            {
                aborted = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        while (touch.getTouch(&tx, &ty))
            vTaskDelay(pdMS_TO_TICKS(20));
        for (int t = 100; t >= 0; t--)
        {
            if (!aborted && touch.getTouch(&tx, &ty))
                aborted = true;
            ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, (t * 1023) / 100);
            ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
            vTaskDelay(pdMS_TO_TICKS(aborted ? 5 : 40));
        }
    }
    xSemaphoreTake(fb_mutex, portMAX_DELAY);
    safe_clear_screen(back_buffer, 0x0000);
    flush_screen_vsync();
    xSemaphoreGive(fb_mutex);

    vTaskDelay(pdMS_TO_TICKS(500));
    BRIGHTNESS = load_brightness_from_NVS(&BRIGHTNESS_NIGHT); // Wczytanie starej jasności

    // 1. Rejestracja zadania wygaszania dwukropka (Priorytet 5)
    xTaskCreate(vTaskDisableColon, "TaskDisableColon", 4096, NULL, 5, &hTaskDisableColon);

    // 2. Rejestracja zadania szybkiego skanowania dotyku (Priorytet 4)
    xTaskCreate(vTaskTouch, "vTaskTouch", 8192, NULL, 4, &hTaskTouch);

    xTaskCreate(vTaskMenuEngine, "vTaskMenuEngine", 8192, NULL, 4, &hTaskMenuEngine);

    printf("[LOG] Podsystem RTC: Zadania mrugania i dotyku zostały pomyślnie zarejestrowane.\n");

    xTaskCreatePinnedToCore(vTaskWeather, "TaskWeather", 6144, NULL, 2, NULL, 1);

    // Uruchomienie cyklicznego automatu synchronizacji NTP (raz na dobę / co 4 godziny)
    // Przypinamy go do rdzenia 1 (ekran działa na rdzeniu 0), dajemy 4096 B stosu i priorytet 2
    BaseType_t xReturned4h = xTaskCreatePinnedToCore(
        vTaskFourHourIncrementer, "Task4hNtp", 4096, NULL, 2, NULL, 1);

    if (xReturned4h == pdPASS)
    {
        printf("[LOG] Podsystem RTC: Licznik 4h / 24h NTP został zarejestrowany pomyślnie.\n");
    }
    else
    {
        printf("[ERROR] Błąd uruchomienia vTaskFourHourIncrementer!\n");
    }

    printf("[LOG] Podsystem RTC: Licznik 4h został zarejestrowany.\n");

    print_RTC();
    vTaskSuspend(hTaskDisableColon);

    uint8_t start_br = (dane_czasu.tm_hour >= 20 || dane_czasu.tm_hour < 6) ? BRIGHTNESS_NIGHT : BRIGHTNESS;
    for (int t = 0; t <= start_br; t++)
    {
        set_backlight_brightness(uint8_t(t));
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    set_backlight_brightness(start_br);
    print_RTC();
    vTaskResume(hTaskDisableColon);
}

//=====================================================================================================
//=====================================================================================================
void tst_lcd(void)
{
    // 4. Jedno, czyste i pewne czyszczenie Cache dla tego bufora
    esp_cache_msync((void *)back_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    xSemaphoreTake(vsync_sem, 0);
    xSemaphoreTake(vsync_sem, portMAX_DELAY);
    lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)back_buffer);
}
//=====================================================================================================

void print_RTC(void)
{
    if (is_editing_time == true)
    {
        return;
    }
    char tekst[32];

    // uint64_t rtc_start = 0;
    // uint64_t rtc_end = 0;

    // rtc_start = esp_timer_get_time();
    // 1. Pobieramy aktualny czas UNIX (liczba sekund od 1970 r.)
    time(&sekundy_epoch);

    // 2. Parsujemy sekundy na czytelną strukturę kalendarzową
    localtime_r(&sekundy_epoch, &dane_czasu);

    // 3. W tym momencie w 'dane_czasu.tm_sec' masz aktualną sekundę (od 0 do 59)
    int second_now = dane_czasu.tm_sec;
    if (old_seconds == second_now)
    {
        return;
    }
    old_seconds = second_now;
    apply_scheduled_brightness(dane_czasu.tm_hour);
    xSemaphoreTake(fb_mutex, portMAX_DELAY);

    snprintf(tekst, sizeof(tekst), "%04d-%02d-%02d", (dane_czasu.tm_year + 1900), (dane_czasu.tm_mon + 1), dane_czasu.tm_mday);
    snprintf(txt_godz, sizeof(txt_godz), "%02d", dane_czasu.tm_hour);
    snprintf(txt_min, sizeof(txt_min), "%02d", dane_czasu.tm_min);
    snprintf(txt_sek, sizeof(txt_sek), "%02d", dane_czasu.tm_sec);

    // 1. Czyszczenie jedynego bufora na czarno
    safe_clear_screen(back_buffer, 0x0000);
    // godziny zakres x 0-460, y 0-370
    // minuty zakres x  555-1023 y 0-370
    // sekundy zakres x 0-200 y 375-480
    // WEEKDAY zakres x 220-1023 y 375-480
    // BRIGHTNESS zakres x 750-1023 y 490-600
    // ROK zakres x 0-250 y 485-600
    // M-C zakres x 260-512 y 485-600
    // DAY zakres x 520-745 y 485-600

    // 2. Rysowanie bazy zegara (czcionki wektorowe bezpośrednio na nim)
    // godziny zakres x 0 - 460, y 0 - 370
    draw_clock_perfect_monospace(back_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_godz, 2, 2, 295, 0xFFFF);
    // minuty zakres x  555 - 1023 y 0 - 370
    draw_clock_perfect_monospace(back_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_min, 555, 2, 295.0f, 0xFFFF); // txt_min
    // colon
    draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 490, 80, 40, 40, 0xFFFF);
    draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 490, 235, 40, 40, 0xFFFF);

    // sekundy zakres x 0-200 y 375-480
    draw_clock_perfect_monospace(back_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_sek, 50, 376, 80.0f, 0xFFFF);
    int week_day = calc_dow((dane_czasu.tm_year + 1900), (dane_czasu.tm_mon + 1), dane_czasu.tm_mday);
    // WEEKDAY zakres x 220-1023 y 375-480
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, getDayofweek(week_day), 210, 345, 160, 0xFFFF);

    // DATA
    draw_clock_perfect_monospace(back_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, tekst, 8, 495, 77.0f, 0xFFFF);
    // BRIGHTNESS zakres x 750-1023 y 490-600
    //**************************************************************************** */
    // OUTDOOR TEMP (w miejscu dawnego paska jasnosci: x 750-1023, y 490-600)
    char txt_temp[16];
    uint16_t frame_color = 0x07E0;
    uint16_t text_color = 0xFFFF;
    if (g_outdoor_temp < -100.0f)
        snprintf(txt_temp, sizeof(txt_temp), "--.-");

    else
    {
        float t_show = (g_outdoor_temp > -0.05f && g_outdoor_temp < 0.0f) ? 0.0f : g_outdoor_temp;
        snprintf(txt_temp, sizeof(txt_temp), "%.1f", t_show);
        if (t_show <= 0.0f)
        {
            frame_color = 0x07FF;
            text_color = 0x07FF;
        }
    }
    if (!temp_read)
        frame_color = 0xF100;
    int len = strlen(txt_temp);
    int num_w = (len * 50) - 22;

    int text_x = 855 - ((num_w + 92) / 2);
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 685, 490, (1024 - 685 - 4), (600 - 490 - 4), 4, frame_color);

    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, txt_temp, text_x, 492, 100.0f, text_color);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, ".", text_x + num_w + 14, 485, 60.0f, text_color);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "C", text_x + num_w + 35, 492, 100.0f, text_color);
    /****************************************************************************** */

    if (ntp_read == true)
    {
        draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 950, 420, 40, 40, 0x07E0);
    }
    else
    {
        draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 950, 420, 40, 40, 0xF100);
    }

    flush_screen_vsync();
    xSemaphoreGive(fb_mutex);

    static int last_triggered_min = -1;

    if (g_slide_enabled && !is_editing_time && dane_czasu.tm_sec == 20)
    {
        if ((dane_czasu.tm_min % g_slide_interval == 0) && (dane_czasu.tm_min != last_triggered_min))
        {
            last_triggered_min = dane_czasu.tm_min;
            g_trigger_auto_photo = true;
        }
    }
    if (dane_czasu.tm_sec != 20)
    {
        last_triggered_min = -1; // Reset blokady po minięciu 20. sekundy
    }

    // 6. Wybudzamy task gaszenia dwukropka (który po 500 ms tylko go zamaluje na czarno)
    xTaskNotifyGive(hTaskDisableColon);

    // rtc_end = esp_timer_get_time();
    // printf("[PROFILER] RENDER RTC (print_RTC) zajął: %llu us\n", (rtc_end - rtc_start));
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void apply_scheduled_brightness(uint8_t hour, bool force)
{
    static uint8_t last_applied_br = 255;
    uint8_t saved_day = BRIGHTNESS;
    bool is_night = (hour >= 20 || hour < 6);
    uint8_t target = is_night ? BRIGHTNESS_NIGHT : BRIGHTNESS;

    if (force || target != last_applied_br)
    {
        set_backlight_brightness(target);
        BRIGHTNESS = saved_day;
        last_applied_br = target;
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void draw_hollow_rectangle(uint16_t *buffer, uint16_t buf_w, uint16_t buf_h,
                           uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           uint16_t thickness, uint16_t color)
{
    // Rysowanie czterech krawędzi prostokąta (górnej, dolnej, lewej i prawej)
    draw_rectangle(buffer, buf_w, buf_h, x, y, w, thickness, color);
    draw_rectangle(buffer, buf_w, buf_h, x, y + h - thickness, w, thickness, color);
    draw_rectangle(buffer, buf_w, buf_h, x, y, thickness, h, color);
    draw_rectangle(buffer, buf_w, buf_h, x + w - thickness, y, thickness, h, color);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_minutes_menu(void)
{
    rx8025t_time_t t;
    if (rx8025t_get_time(&t) != ESP_OK)
    {
        is_editing_time = false;
        return;
    }
    uint8_t new_min = edit_number_menu("SET MINUTS", 280, t.minute, 0, 59, false);
    if (rx8025t_get_time(&t) == ESP_OK)
    {
        t.minute = new_min;
        if (rx8025t_set_time(&t) == ESP_OK)
        {
            sync_internal_rtc_with_rx8025t();
        }
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void wait_for_release(void)
{
    // Sprawdzamy stan wątku sprzętowego w tle
    eTaskState state = eTaskGetState(hTaskHardwareTouchDriver);

    // Jeśli wątek sprzętowy NIE jest zawieszony (czyli działa i sam odpytuje I2C)
    if (state != eSuspended && state != eDeleted)
    {
        // Poziom 2: Bezpiecznie czekamy na flagę w pamięci RAM
        while (global_is_touched == true)
        {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
    else
    {
        // Poziom 1: Driver w tle śpi, więc główny wątek może bezpiecznie odpytać I2C
        uint16_t dummyX = 0, dummyY = 0;
        while (touch.getTouch(&dummyX, &dummyY) == true)
        {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
}

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void draw_test_cross(void)
{
    // TYMCZASOWE LINIE POMOCNICZE DO POZYCJONOWANIA (Kolor Czerwony: 0xF800)
    // 1. Linia pionowa dzieląca szerokość (1024) na pół w punkcie X = 512
    draw_line(back_buffer, LCD_H_RES, LCD_V_RES, 512, 0, 599, 0xF800);

    // 2. Linia pozioma dzieląca wysokość (600) na pół w punkcie Y = 300
    // Rysujemy ją jako cienki prostokąt od lewej do prawej krawędzi (od X=0 do 1024)
    draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 0, 300, 1024, 2, 0xF800);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void show_debug_touch(uint16_t touchX, uint16_t touchY)
{
    char txt_debug_touch[32];
    snprintf(txt_debug_touch, sizeof(txt_debug_touch), "X:%d Y:%d", touchX, touchY);
    draw_clock_perfect_monospace(back_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_debug_touch, 10, 10, 26.0f, 0x07E0);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void vTaskMenuEngine(void *pvParameters)
{
    uint32_t menu_choice = 0;

    printf("[SYSTEM] Zadanie vTaskMenuEngine gotowe i zablokowane na kolejce powiadomień.\n");
    while (1)
    {
        // Funkcja czeka na powiadomienie i zapisuje przekazaną liczbę do zmiennej
        xTaskNotifyWait(0x00, ULONG_MAX, &menu_choice, portMAX_DELAY);
        xSemaphoreTake(fb_mutex, portMAX_DELAY);
        if (hTaskDisableColon != NULL)
        {
            xTaskNotifyStateClear(hTaskDisableColon);
        }
        if (hTaskDisableColon != NULL)
        {
            vTaskSuspend(hTaskDisableColon);
        }

        // --- TWARDE CZYSZCZENIE SPRZĘTOWEGO POTOKU PRZED MENU ---
        safe_clear_screen(back_buffer, 0x0000);
        flush_screen_vsync();
        xSemaphoreGive(fb_mutex);

        vTaskDelay(pdMS_TO_TICKS(40)); // Dajemy czas na pełne przełączenie ekranu

        if (menu_choice == 1)
        {
            set_minutes_menu();
        }
        else if (menu_choice == 2)
        {
            set_hours_menu();
        }
        else if (menu_choice == 3)
        {
            set_light_menu();
        }
        else if (menu_choice == 4) // NOWOŚĆ: POWIADOMIENIE NR 4
        {
            set_day_menu();
        }
        else if (menu_choice == 5) // NOWOŚĆ: POWIADOMIENIE NR 5
        {
            set_month_menu();
        }
        else if (menu_choice == 6) // USTAWIENIE ROKU
        {
            set_year_menu();
        }
        else if (menu_choice == 7) // NOWOŚĆ: WYWOŁANIE MENU NTP
        {
            set_ntp_and_demo_menu();
        }

        // --- KLUCZOWY FIX PO WCIŚNIĘCIU DONE ---
        // ====================================================================
        // GLOBALNY SPRZĘTOWY RESET POTOKU PO WYJŚCIU Z JAKIEGOKOLWIEK MENU
        // ====================================================================
        xSemaphoreTake(fb_mutex, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(20));

        // 2. Czyścimy bufor tła na idealną czerń i przełączamy klatkę
        safe_clear_screen(back_buffer, 0x0000);
        flush_screen_vsync();

        xSemaphoreGive(fb_mutex);

        old_seconds = 111;
        is_editing_time = false;
        apply_scheduled_brightness(dane_czasu.tm_hour, true);

        // 5. Oczyszczamy zaległe powiadomienia i budzimy dwukropek w stanie czystym
        if (hTaskDisableColon != NULL)
        {
            uint32_t ulDummy;
            xTaskNotifyStateClear(hTaskDisableColon);
            xTaskNotifyWait(0x00, ULONG_MAX, &ulDummy, 0); // Błyskawiczne opróżnienie kolejki powiadomień bez czekania (tick = 0)
            vTaskResume(hTaskDisableColon);
        }

        // 6. Budzimy skanowanie ekranu głównego zegara
        printf("[ENGINE] Menu zakończone globalnie. Wznawiam vTaskTouch...\n");
        if (hTaskTouch != NULL)
        {
            vTaskResume(hTaskTouch);
        }
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void draw_common_menu_base(void)
{
    // MINUS X 40-240 Y 165-390
    // PLUS  784-984  165-390
    // DONE  312-712 450-540

    // A. Czyszczenie ekranu na czarno
    safe_clear_screen(back_buffer, 0x0000);

    // D. Rysowanie elementów interfejsu (Przyciski sterujące: PLUS, MINUS, DONE)
    // Format: draw_hollow_rectangle(buffer, buf_w, buf_h, x, y, w, h, grubość, kolor)
    // Rysowanie ramki MINUS (od X=50, Y=220, szerokość=250, wysokość=300)
    draw_clock_perfect_monospace(back_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, "-", 90, 210, 120.0f, 0xFFFF);
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 40, 165, 200, 225, 4, 0xFFFF);

    // Rysowanie ramki PLUS (od X=980, Y=220, szerokość=250, wysokość=300)
    draw_clock_perfect_monospace(back_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, "+", 840, 210, 120.0f, 0xFFFF);
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 784, 165, 200, 225, 4, 0xFFFF);

    // Rysowanie ramki DONE (od X=390, Y=600, szerokość=500, wysokość=120)
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "DONE", 400, 430, 120.0f, 0xFFFF);
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 312, 450, 400, 90, 4, 0xFFFF);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_hours_menu(void)
{
    rx8025t_time_t t;
    if (rx8025t_get_time(&t) != ESP_OK)
    {
        is_editing_time = false;
        return;
    }
    uint8_t new_h = edit_number_menu("SET HOURS", 300, t.hour, 0, 23, false);
    if (rx8025t_get_time(&t) == ESP_OK)
    {
        t.hour = new_h;
        if (rx8025t_set_time(&t) == ESP_OK)
        {
            sync_internal_rtc_with_rx8025t();
        }
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void reset_seconds_to_zero(void)
{
    rx8025t_time_t final_time;

    // 1. Pobieramy AKTUALNY pełny stan rejestrów z chipu RX8025T
    if (rx8025t_get_time(&final_time) == ESP_OK)
    {
        // 2. Zerujemy sekundy w strukturze
        final_time.second = 59;

        // 3. Zapisujemy zaktualizowaną strukturę z zerem z powrotem do układu RX8025T
        if (rx8025t_set_time(&final_time) == ESP_OK)
        {
            printf("[RTC] Sekundy wyzerowane w układzie zewnętrznym RX8025T.\n");

            // 4. Synchronizujemy wbudowane, wewnętrzne RTC ESP32P4 z nowymi danymi z kostki
            sync_internal_rtc_with_rx8025t();
            printf("[RTC] Wewnętrzne RTC ESP32P4 zsynchronizowane z nową bazą czasu.\n");
        }
        else
        {
            printf("[ERROR] Nie udało się zapisać wyzerowanych sekund do RX8025T!\n");
        }
    }
    else
    {
        printf("[ERROR] Brak komunikacji z RX8025T przy próbie zerowania sekund!\n");
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_day_menu(void)
{
    wait_for_release();
    rx8025t_time_t t;
    if (rx8025t_get_time(&t) != ESP_OK)
    {
        is_editing_time = false;
        return;
    }
    uint8_t max_d = get_max_days_in_month((t.year < 2000) ? t.year + 2000 : t.year, t.month);
    uint8_t new_d = edit_number_menu("SET DAY", 280, t.day, 1, max_d, false);
    if (rx8025t_get_time(&t) == ESP_OK)
    {
        t.day = new_d;
        if (rx8025t_set_time(&t) == ESP_OK)
        {
            sync_internal_rtc_with_rx8025t();
        }
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_year_menu(void)
{
    rx8025t_time_t t;
    if (rx8025t_get_time(&t) != ESP_OK)
    {
        is_editing_time = false;
        return;
    }
    int start_y = (t.year < 2000) ? t.year + 2000 : t.year;
    int new_y = edit_number_menu("SET YEAR", 280, start_y, 2026, 2099, true);
    if (rx8025t_get_time(&t) == ESP_OK)
    {
        t.year = new_y;
        uint8_t max_d = get_max_days_in_month(t.year, t.month);
        if (t.day > max_d)
        {
            t.day = max_d;
        }
        if (rx8025t_set_time(&t) == ESP_OK)
        {
            sync_internal_rtc_with_rx8025t();
        }
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_month_menu(void)
{
    wait_for_release();
    rx8025t_time_t t;
    if (rx8025t_get_time(&t) != ESP_OK)
    {
        is_editing_time = false;
        return;
    }
    uint8_t new_m = edit_number_menu("SET MONTH", 280, t.month, 1, 12, false);
    if (rx8025t_get_time(&t) == ESP_OK)
    {
        t.month = new_m;
        uint8_t max_d = get_max_days_in_month((t.year < 2000) ? t.year + 2000 : t.year, t.month);
        if (t.day > max_d)
        {
            t.day = max_d;
        }
        if (rx8025t_set_time(&t) == ESP_OK)
        {
            sync_internal_rtc_with_rx8025t();
        }
    }
}

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// Uniwersalna funkcja do ustawiania jasności (0 - 100%)
void set_backlight_brightness(uint8_t percentage)
{
    if (percentage > 100)
        percentage = 100;
    if (percentage < 5)
        percentage = 5;
    // Przeliczenie procentów (0-100) na wartość 10-bitową (0-1023)
    uint32_t duty = (percentage * 1023) / 100;

    ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, duty);
    ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);

    // printf("[PWM] Jasność zmieniona na: %d%%\n", percentage);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void reconfig_pin_bl_to_pwm(void)
{
    // 1. Konfiguracja timera LEDC - ZWRÓĆ UWAGĘ NA DOPISANE .deconfigure NA KOŃCU!
    ledc_timer_config_t ledc_timer = {
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .duty_resolution = BACKLIGHT_LEDC_DUTY_RES,
        .timer_num = BACKLIGHT_LEDC_TIMER,
        .freq_hz = BACKLIGHT_LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false // <<< TA LINIA USUNIE WARNING Z LINII 819!
    };
    ledc_timer_config(&ledc_timer);

    // 2. Konfiguracja kanału LEDC - ZWRÓĆ UWAGĘ NA DOPISANE .sleep_mode ORAZ .flags NA KOŃCU!
    ledc_channel_config_t ledc_channel = {
        .gpio_num = BACKLIGHT_GPIO,
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .channel = BACKLIGHT_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BACKLIGHT_LEDC_TIMER,
        .duty = 1023, // Na start ustawiamy 100% jasności (10-bit max)
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD, // <<< TA LINIA USUNIE WARNING O sleep_mode!
        .flags = {.output_invert = 0}                 // <<< TA LINIA USUNIE WARNING O flags!
    };
    ledc_channel_config(&ledc_channel);

    printf("[PWM] Pin 23 został pomyślnie REKONFIGUROWANY na tryb PWM (10-bit, 5kHz).\n");
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// Definicja globalnej zmiennej dla jasności (zakres 2 - 100)
void set_light_menu(void)
{
    uint16_t touchX = 0, touchY = 0;
    bool content_changed = true;

    safe_clear_screen(back_buffer, 0x0000);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "SET LIGHT", 344, 0, 80.0f, 0xFFFF);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "DAY 06:00-19:59", 308, (94 - 18), (36.0f * 1.5), 0x07E0);
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 106, 135, 814, 94, 4, 0xFFFF);

    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "NIGHT 20:00-05:59", 284, (248 - 18), 54.0f, 0x07FF);
    // draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "NIGHT 20:00-05:59", 360, 248, 36.0f, 0x07FF);
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 106, 289, 814, 94, 4, 0xFFFF);

    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 312, 450, 400, 90, 4, 0xFFFF);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "DONE", 400, 434, 120.0f, 0xFFFF);

    flush_screen_vsync();
    memcpy(back_buffer, (void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2);
    while (1)
    {
        if (!touch.getTouch(&touchX, &touchY))
        {
            touchX = 0;
            touchY = 0;
        }
        if (content_changed)
        {
            xSemaphoreTake(fb_mutex, portMAX_DELAY);
            uint16_t len_day = BRIGHTNESS * 8;
            draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 113, 142, len_day, 80, 0x07E0);
            draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 113 + len_day, 142, 800 - len_day, 80, 0x0000);
            char txt_pct[16];
            snprintf(txt_pct, sizeof(txt_pct), "%d%%", BRIGHTNESS);
            draw_text(back_buffer, LCD_H_RES, LCD_V_RES, txt_pct, 476, 146, 72.0f, 0xFFFF);
            uint16_t len_night = BRIGHTNESS_NIGHT * 8;
            draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 113, 296, len_night, 80, 0x07FF);
            draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 113 + len_night, 296, 800 - len_night, 80, 0x0000);
            snprintf(txt_pct, sizeof(txt_pct), "%d%%", BRIGHTNESS_NIGHT);
            draw_text(back_buffer, LCD_H_RES, LCD_V_RES, txt_pct, 476, 300, 72.0f, 0xFFFF);
            flush_screen_vsync();
            memcpy(back_buffer, (void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2);
            content_changed = false;
            xSemaphoreGive(fb_mutex);
        }
        if (touchX > 0 && touchY > 0)
        {
            if (touchX >= 106 && touchX <= 920 && touchY >= 135 && touchY <= 229)
            {
                int val = (touchX - 113) / 8;
                BRIGHTNESS = (val < 5) ? 5 : (val > 100 ? 100 : val);
                set_backlight_brightness(BRIGHTNESS);
                content_changed = true;
            }
            else if (touchX >= 106 && touchX <= 920 && touchY >= 289 && touchY <= 383)
            {
                int val = (touchX - 113) / 8;
                uint8_t old_day = BRIGHTNESS;
                BRIGHTNESS_NIGHT = (val < 5) ? 5 : (val > 100 ? 100 : val);
                set_backlight_brightness(BRIGHTNESS_NIGHT);
                BRIGHTNESS = old_day;
                content_changed = true;
            }
            else if (touchX >= 312 && touchX <= 712 && touchY >= 450 && touchY <= 540)
            {
                save_brightness_to_NVS(BRIGHTNESS, BRIGHTNESS_NIGHT);
                apply_scheduled_brightness(dane_czasu.tm_hour, true);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    wait_for_release();
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// TWOJA FUNKCJA Z STM32 ZWRACAJĄCA WSKAŹNIK NA STRINGS DNIA TYGODNIA (ZAKRES OTRZYMANY 1-7)
//=====================================================================================================
const char *getDayofweek(uint8_t number)
{
    if (number == 0)
    {
        number = 1;
    }
    if (number > 7)
    {
        number = 7;
    }

    // TABLICA STATYCZNA CONST DLA BEZPIECZEŃSTWA PAMIĘCI W C++
    static const char *weekday[] = {"MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY", "SUNDAY"};
    return weekday[number - 1];
}

//=====================================================================================================
// TWÓJ SPRAWDZONY ALGORYTM LICZENIA DNIA TYGODNIA (ZWRACA 1 = PONIEDZIAŁEK, ..., 7 = NIEDZIELA)
//=====================================================================================================
int calc_dow(int y, int m, int d)
{
    int result;
    y -= m < 3;
    result = (y + y / 4 - y / 100 + y / 400 + "-bed=pen+mad."[m] + d) % 7;
    if (result == 0)
    {
        result = 7;
    }
    return result;
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// BEZPIECZNA INICJALIZACJA PAMIĘCI NVS (WYWOŁYWANA RAZ NA STARCIE PROJEKTU)
//=====================================================================================================
void init_NVS_system(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // Jeśli pamięć jest uszkodzona lub ma złą wersję, czyścimy ją i stawiamy od nowa
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    printf("[NVS] Podsystem pamięci nieulotnej został pomyślnie zainicjalizowany.\n");
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
// ZAPIS WARTOŚCI JASNOŚCI DO PAMIĘCI NIEULOTNEJ NVS
//=====================================================================================================
void save_brightness_to_NVS(uint8_t day_val, uint8_t night_val)
{
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK)
    {
        uint8_t old_d = 255, old_n = 255;
        bool dirty = false;
        if (nvs_get_u8(my_handle, "bl_light", &old_d) != ESP_OK || old_d != day_val)
        {
            nvs_set_u8(my_handle, "bl_light", day_val);
            dirty = true;
        }
        if (nvs_get_u8(my_handle, "bl_night", &old_n) != ESP_OK || old_n != night_val)
        {
            nvs_set_u8(my_handle, "bl_night", night_val);
            dirty = true;
        }
        if (dirty)
            nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
// ODCZYT JASNOŚCI Z PAMIĘCI NVS (JEŚLI BRAK WPISU, DAJE WARTOŚĆ DOMYŚLNĄ)
//=====================================================================================================
uint8_t load_brightness_from_NVS(volatile uint8_t *night_out)
{
    nvs_handle_t my_handle;
    uint8_t saved_day = 50, saved_night = 20;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK)
    {
        if (nvs_get_u8(my_handle, "bl_light", &saved_day) != ESP_OK)
            saved_day = 50;
        if (nvs_get_u8(my_handle, "bl_night", &saved_night) != ESP_OK)
            saved_night = 20;
        nvs_close(my_handle);
    }
    if (night_out)
        *night_out = saved_night;
    return saved_day;
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// ZWRACA MAKSYMALNĄ LICZBĘ DNI DLA PODANEGO MIESIĄCA I ROKU (UWZGLĘDNIA LATA PRZESTĘPNE)
//=====================================================================================================
uint8_t get_max_days_in_month(int year, uint8_t month)
{
    if (month == 2)
    {
        // Rok przestępny: podzielny przez 4, ale nie przez 100, chyba że podzielny przez 400
        if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))
        {
            return 29;
        }
        return 28;
    }
    // Kwiecień, Czerwiec, Wrzesień, Listopad mają 30 dni
    if (month == 4 || month == 6 || month == 9 || month == 11)
    {
        return 30;
    }
    return 31;
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void init_spiffs_storage(void)
{
    printf("[SPIFFS] Inicjalizacja i montowanie partycji 'storage'...\n");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",        // Ścieżka dostępu w kodzie: /spiffs/
        .partition_label = "storage",  // Musi pasować do partitions.csv
        .max_files = 5,                // Maksymalna liczba otwartych plików
        .format_if_mount_failed = true // Sformatuje przy pierwszym starcie
    };

    // Rejestracja dysku w wirtualnym systemie plików
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            printf("[SPIFFS_ERROR] Nie udalo sie zamontowac ani sformatowac systemu plikow!\n");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            printf("[SPIFFS_ERROR] Nie znaleziono partycji o etykiecie 'storage'!\n");
        }
        else
        {
            printf("[SPIFFS_ERROR] Blad inicjalizacji SPIFFS (%s)\n", esp_err_to_name(ret));
        }
        return;
    }

    // Pobranie informacji o wielkości dysku
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret == ESP_OK)
    {
        printf("[SPIFFS_SUCCESS] Partycja zamontowana! Rozmiar: %d KB, Uzyto: %d KB\n", total / 1024, used / 1024);

        // TWARDE I JAWNE SPRAWDZENIE TWOICH ZDJĘĆ Z FOLDERU FOTOS
        FILE *f1 = fopen("/spiffs/FOTO1.jpg", "r");
        FILE *f2 = fopen("/spiffs/FOTO2.jpg", "r");

        if (f1 != NULL)
        {
            printf("[SPIFFS_TEST] SUKCES: Wykryto plik -> /spiffs/FOTO1.jpg\n");
            fclose(f1);
        }
        else
        {
            printf("[SPIFFS_TEST] BLAD: Brak pliku FOTO1.jpg na partycji Flash!\n");
        }

        if (f2 != NULL)
        {
            printf("[SPIFFS_TEST] SUKCES: Wykryto plik -> /spiffs/FOTO2.jpg\n");
            fclose(f2);
        }
        else
        {
            printf("[SPIFFS_TEST] BLAD: Brak pliku FOTO2.jpg na partycji Flash!\n");
        }
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
int edit_number_menu(const char *title, int title_x, int start_val, int min_val, int max_val, bool is_year)
{
    // uint16_t old_touchX = 9999;
    // uint16_t old_touchY = 9999;
    // !!!
    char txt_val[16];
    uint16_t touchX = 0, touchY = 0;
    int hold_counter = 0, val = start_val;
    bool content_changed = true;

    draw_common_menu_base();
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, title, title_x, 16, 120.0f, 0xFFFF);
    snprintf(txt_val, sizeof(txt_val), is_year ? "%04d" : "%02d", val);
    draw_clock_perfect_monospace(back_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_val, is_year ? 280 : 375, 185, is_year ? 140.0f : 175.0f, 0xFFFF);
    flush_screen_vsync();
    memcpy(back_buffer, (void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2);

    global_is_touched = false;
    global_touchX = 0;
    global_touchY = 0;
    vTaskResume(hTaskHardwareTouchDriver);
    wait_for_release();
    vTaskDelay(pdMS_TO_TICKS(100));

    while (is_editing_time)
    {
        touchX = global_touchX;
        touchY = global_touchY;

        // MINUS X 40-240 Y 165-390
        // PLUS  784-984  165-390
        // DONE  312-712 450-540

        if (touchX > 0 && touchY > 0)
        {
            // MINUS button pressed
            // if (touchX >= 200 && touchX <= 600 && touchY >= 50 && touchY <= 350 && (hold_counter == 1 || (hold_counter > 500 && hold_counter % 15 == 0)))
            if (touchX >= 40 && touchX <= 240 && touchY >= 165 && touchY <= 390 && (hold_counter == 1 || (hold_counter > 10 && hold_counter % 2 == 0)))
            {
                val = (val <= min_val) ? max_val : val - 1;
                content_changed = true;
            }
            // PLUS button pressed
            // else if (touchX >= 200 && touchX <= 600 && touchY >= 950 && touchY <= 1250 && (hold_counter == 1 || (hold_counter > 500 && hold_counter % 15 == 0)))
            else if (touchX >= 784 && touchX <= 984 && touchY >= 165 && touchY <= 390 && (hold_counter == 1 || (hold_counter > 10 && hold_counter % 2 == 0)))
            {
                val = (val >= max_val) ? min_val : val + 1;
                content_changed = true;
            }
            // DONE button pressed
            else if (touchX >= 312 && touchX <= 712 && touchY >= 450 && touchY <= 540)
            {
                vTaskSuspend(hTaskHardwareTouchDriver);
                break;
            }

            hold_counter++;
        }
        else
        {

            hold_counter = 0;
        }

        // =========================================================================
        // NIEZALEŻNE ODŚWIEŻANIE DEBUGERA DOTYKU (BEZ ZAKŁÓCANIA REPETYCJI!)
        // =========================================================================
        bool need_flush = content_changed;

        // if (touchX > 0 && touchY > 0)
        // {
        //     if (touchX != old_touchX || touchY != old_touchY)
        //     {
        //         old_touchX = touchX;
        //         old_touchY = touchY;
        //         // Rysujemy debuger dotyku (pole o szerokości 300 idealnie mieści się przed ramką minusa)
        //         draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 7, 7, 300, 35, 0x0000);
        //         show_debug_touch(touchX, touchY);
        //         need_flush = true;
        //     }
        // }

        // Obsługa zmiany cyfry

        if (content_changed)
        {
            snprintf(txt_val, sizeof(txt_val), is_year ? "%04d" : "%02d", val);
            draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 244, 150, 540, 260, 0x0000);
            draw_clock_perfect_monospace(back_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_val, is_year ? 280 : 375, 185, is_year ? 140.0f : 175.0f, 0xFFFF);
            content_changed = false;
        }

        if (need_flush)
        {
            flush_screen_vsync();
            memcpy(back_buffer, (void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2);
            content_changed = false;
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
    vTaskSuspend(hTaskHardwareTouchDriver);
    wait_for_release();
    return val;
}

// int edit_number_menu(const char *title, int title_x, int start_val, int min_val, int max_val, bool is_year)
// {
//     // uint16_t old_touchX = 9999;
//     // uint16_t old_touchY = 9999;

//     uint16_t touchX = 0, touchY = 0;
//     int hold_counter = 0, val = start_val;

//     bool content_changed = true;

//     draw_common_menu_base();
//     draw_text(back_buffer, LCD_H_RES, LCD_V_RES, title, title_x, 16, 120.0f, 0xFFFF);

//     char txt_init[16];
//     snprintf(txt_init, sizeof(txt_init), is_year ? "%04d" : "%02d", val);
//     draw_clock_perfect_monospace(back_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_init, is_year ? 280 : 375, 185, is_year ? 140.0f : 175.0f, 0xFFFF);

//     flush_screen_vsync();
//     memcpy(back_buffer, (void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2);

//     draw_text(back_buffer, LCD_H_RES, LCD_V_RES, title, title_x, 16, 120.0f, 0xFFFF);
//     flush_screen_vsync();
//     memcpy(back_buffer, (void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2);

//     global_is_touched = false;
//     global_touchX = 0;
//     global_touchY = 0;
//     vTaskResume(hTaskHardwareTouchDriver);

//     wait_for_release();
//     vTaskDelay(pdMS_TO_TICKS(100)); // Dajemy czas na fizyczne przemieszczenie kontrolera i zdejmujemy blokadę czasu

//     int release_debounce = 0;
//     while (is_editing_time)
//     {
//         touchX = global_touchX;
//         touchY = global_touchY;

//         // MINUS X 40-240 Y 165-390
//         // PLUS  784-984  165-390
//         // DONE  312-712 450-540

//         if (touchX > 0 && touchY > 0)
//         {
//             release_debounce = 0;
//             // MINUS button pressed
//             // if (touchX >= 200 && touchX <= 600 && touchY >= 50 && touchY <= 350 && (hold_counter == 1 || (hold_counter > 500 && hold_counter % 15 == 0)))
//             if (touchX >= 40 && touchX <= 240 && touchY >= 165 && touchY <= 390 && (hold_counter == 1 || (hold_counter > 25 && hold_counter % 3 == 0)))
//             {
//                 val = (val <= min_val) ? max_val : val - 1;
//                 content_changed = true;
//             }
//             // PLUS button pressed
//             // else if (touchX >= 200 && touchX <= 600 && touchY >= 950 && touchY <= 1250 && (hold_counter == 1 || (hold_counter > 500 && hold_counter % 15 == 0)))
//             else if (touchX >= 784 && touchX <= 984 && touchY >= 165 && touchY <= 390 && (hold_counter == 1 || (hold_counter > 25 && hold_counter % 3 == 0)))
//             {
//                 val = (val >= max_val) ? min_val : val + 1;
//                 content_changed = true;
//             }
//             // DONE button pressed
//             else if (touchX >= 312 && touchX <= 712 && touchY >= 450 && touchY <= 540)
//             {
//                 vTaskSuspend(hTaskHardwareTouchDriver);
//                 break;
//             }

//             hold_counter++;
//         }
//         else
//         {
//             if (release_debounce < 4) // Tolerujemy do 4 pustych odczytów (~40 ms) z GT911
//             {
//                 release_debounce++;
//             }
//             else
//             {
//                 hold_counter = 0;
//             }
//         }

//         // =========================================================================
//         // NIEZALEŻNE ODŚWIEŻANIE DEBUGERA DOTYKU (BEZ ZAKŁÓCANIA REPETYCJI!)
//         // =========================================================================
//         bool need_flush = content_changed;
//         /*
//                 if (touchX > 0 && touchY > 0)
//                 {
//                     if (touchX != old_touchX || touchY != old_touchY)
//                     {
//                         old_touchX = touchX;
//                         old_touchY = touchY;
//                         draw_rectangle_90(back_buffer, LCD_H_RES, LCD_V_RES, 10, 10, 330, 37, 0x0000);
//                         show_debug_touch(touchX, touchY);
//                         need_flush = true;
//                     }
//                 }
//                 */
//         // Obsługa zmiany cyfry

//         if (content_changed)
//         {
//             char txt_val[16];
//             snprintf(txt_val, sizeof(txt_val), is_year ? "%04d" : "%02d", val);
//             draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 244, 150, 540, 260, 0x0000);
//             draw_clock_perfect_monospace(back_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_val, is_year ? 280 : 375, 185, is_year ? 140.0f : 175.0f, 0xFFFF);
//             content_changed = false;
//         }

//         if (need_flush)
//         {
//             flush_screen_vsync();
//             memcpy(back_buffer, (void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2);
//             content_changed = false;
//         }

//         vTaskDelay(pdMS_TO_TICKS(15));
//     }
//     vTaskSuspend(hTaskHardwareTouchDriver);
//     wait_for_release();
//     return val;
// }
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void wyswietl_jpeg_ze_spiffs(const char *sciezka_pliku)
{
    xSemaphoreTake(fb_mutex, portMAX_DELAY);
    printf("[JPEG] Odczyt pliku: %s\n", sciezka_pliku);
    FILE *f = fopen(sciezka_pliku, "rb");
    if (f == NULL)
    {
        printf("[JPEG_ERROR] Nie mozna otworzyc pliku: %s\n", sciezka_pliku);
        xSemaphoreGive(fb_mutex);
        return;
    }

    // 1. Sprawdzamy rozmiar pliku JPEG
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    const size_t max_jpg_bytes = 1024 * 1024; // Poza buforem obrazu: bufor na plik .jpg

    if (file_size <= 0 || (size_t)file_size > max_jpg_bytes)
    {
        printf("[JPEG_ERROR] Plik za duzy (%ld B) lub pusty! Limit to 1 MB.\n", file_size);
        fclose(f);
        xSemaphoreGive(fb_mutex);
        return;
    }

    // 2. Wczytujemy plik .jpg dokladnie na poczatek trzeciego megabajta (offset +2 MB)
    uint8_t *base_ptr = (uint8_t *)jpg_scratch_buf;
    uint8_t *jpeg_input_ptr = base_ptr;

    size_t read_bytes = fread(jpeg_input_ptr, 1, file_size, f);
    fclose(f);

    if (read_bytes != (size_t)file_size)
    {
        printf("[JPEG_ERROR] Blad odczytu pliku ze SPIFFS!\n");
        xSemaphoreGive(fb_mutex);
        return;
    }

    // 3. Konfiguracja dekodera esp_new_jpeg (bezposrednio do RGB565_LE)
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.rotate = JPEG_ROTATE_0D;

    jpeg_dec_handle_t jpeg_dec = NULL;
    if (jpeg_dec_open(&config, &jpeg_dec) != JPEG_ERR_OK)
    {
        printf("[JPEG_ERROR] Nie udalo sie zainicjalizowac dekodera JPEG!\n");
        xSemaphoreGive(fb_mutex);
        return;
    }

    jpeg_dec_io_t jpeg_io = {};
    jpeg_io.inbuf = jpeg_input_ptr;
    jpeg_io.inbuf_len = file_size;

    jpeg_dec_header_info_t out_info = {};
    if (jpeg_dec_parse_header(jpeg_dec, &jpeg_io, &out_info) != JPEG_ERR_OK)
    {
        printf("[JPEG_ERROR] Nieprawidlowy naglowek pliku JPEG!\n");
        jpeg_dec_close(jpeg_dec);
        xSemaphoreGive(fb_mutex);
        return;
    }

    // Dekodujemy zbufora
    jpeg_io.outbuf = (uint8_t *)back_buffer;

    if (jpeg_dec_process(jpeg_dec, &jpeg_io) != JPEG_ERR_OK)
    {
        printf("[JPEG_ERROR] Blad podczas dekompresji obrazu JPEG!\n");
        jpeg_dec_close(jpeg_dec);
        xSemaphoreGive(fb_mutex);
        return;
    }

    jpeg_dec_close(jpeg_dec);

    flush_screen_vsync();
    printf("[JPEG_SUCCESS] Obraz %dx%d zdekompresowany i wyslany na LCD!\n", out_info.width, out_info.height);
    xSemaphoreGive(fb_mutex);
}

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// JEDNORAZOWY ZAPIS SSID I HASLA DO PAMIECI NIEULOTNEJ SPIFFS
bool save_wifi_credentials_to_SPIFFS(const char *ssid, const char *password)
{
    FILE *f = fopen("/spiffs/prefs.ini", "w");
    if (f == NULL)
    {
        printf("[SPIFFS_WIFI_ERROR] Nie mozna otworzyc pliku /spiffs/prefs.ini do zapisu!\n");
        return false;
    }

    // Zapisujemy SSID w pierwszej linii oraz hasło w drugiej linii
    fprintf(f, "%s\n%s\n", ssid, password);
    fclose(f); // Zawsze zamykamy plik, aby fizycznie zrzucić dane z bufora do Flash!

    printf("[SPIFFS_WIFI] Zapisano dane Wi-Fi do /spiffs/prefs.ini (SSID: %s)\n", ssid);
    return true;
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// ODCZYT SSID I HASLA Z PLIKU /spiffs/prefs.ini
bool load_wifi_credentials_from_SPIFFS(char *ssid_out, size_t ssid_max_len, char *pass_out, size_t pass_max_len)
{
    FILE *f = fopen("/spiffs/prefs.ini", "r");
    if (f == NULL)
    {
        printf("[SPIFFS_WIFI] Brak pliku /spiffs/prefs.ini!\n");
        return false;
    }

    // Czytamy 1. linię (SSID) oraz 2. linię (Hasło)
    bool ok1 = (fgets(ssid_out, ssid_max_len, f) != NULL);
    bool ok2 = (fgets(pass_out, pass_max_len, f) != NULL);
    fclose(f);

    if (!ok1 || !ok2)
    {
        printf("[SPIFFS_WIFI] Plik /spiffs/prefs.ini jest pusty lub niekompletny!\n");
        return false;
    }

    // Bezpieczne obcięcie znaków nowej linii (\r oraz \n) z Windowsa i Linuxa
    ssid_out[strcspn(ssid_out, "\r\n")] = '\0';
    pass_out[strcspn(pass_out, "\r\n")] = '\0';

    if (strlen(ssid_out) == 0)
    {
        printf("[SPIFFS_WIFI] Pusty SSID w pliku prefs.ini!\n");
        return false;
    }

    printf("[SPIFFS_WIFI] Odczytano SSID z SPIFFS: %s\n", ssid_out);
    return true;
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void print_internal_ram_stats(const char *etap)
{
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t min_free_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    printf("[SRAM_STAT - %s] Wolne teraz: %u KB (%u B) | Najmniej w historii: %u KB | Najwiekszy blok: %u KB\n",
           etap,
           free_internal / 1024,
           free_internal,
           min_free_internal / 1024,
           largest_block / 1024);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void vTaskFourHourIncrementer(void *pvParameters)
{
    printf("[SYSTEM] Zadanie cykliczne NTP (24h / 4h) wystartowało.\n");

    // Licznik wybudzeń (6 wywołań * 4 godziny = 24 godziny)
    static int licznik_4h = 0;
    static bool ostatnia_sync_udana = false;

    // Po restarcie urządzenia wykonujemy pierwszą natychmiastową próbę synchronizacji
    start_ntp_sync_task_async();

    while (1)
    {
        // Zadanie śpi przez dokładnie 4 godziny
        vTaskDelay(pdMS_TO_TICKS(4 * 60 * 60 * 1000));

        licznik_4h++;

        // Flaga sprawdzająca, czy to czas na dobową synchronizację
        bool czas_na_dobowa_sync = (licznik_4h >= 6);

        // Jeśli minęła doba LUB poprzednia próba zakończyła się błędem:
        if (czas_na_dobowa_sync || !ostatnia_sync_udana)
        {
            printf("[HARMONOGRAM] Uruchamiam cykliczne zadanie NTP (Czas na dobową: %d, Stan poprzedniej: %d)\n",
                   czas_na_dobowa_sync, ostatnia_sync_udana);

            if (!is_ntp_sync_running())
            {
                start_ntp_sync_task_async();
                while (is_ntp_sync_running())
                {
                    vTaskDelay(pdMS_TO_TICKS(200));
                }

                time_t teraz;
                struct tm info;
                time(&teraz);
                localtime_r(&teraz, &info);

                if (ntp_read == true)
                {
                    ostatnia_sync_udana = true;
                    licznik_4h = 0; // Sukces! Resetujemy licznik dobowy
                    printf("[HARMONOGRAM] Cykliczna synchronizacja udana. Następna próba za 24h.\n");
                }
                else
                {
                    ostatnia_sync_udana = false;
                    printf("[HARMONOGRAM] Cykliczna synchronizacja nieudana! Ponowienie nastąpi za 4 godziny.\n");
                }
            }
        }
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_ntp_menu(void)
{
    printf("[MENU] -> Uruchomiono graficzne Menu synchronizacji NTP.\n");
    wait_for_release();            // Czekamy na puszczenie ekranu przed wejściem do menu, aby uniknąć natychmiastowego wyzwolenia przycisku
    vTaskDelay(pdMS_TO_TICKS(50)); // Dajemy czas na fizyczne przemieszczenie kontrolera i zdejmujemy blokadę czasu

    //  WAIT FOR DMA
    xSemaphoreTake(vsync_sem, 0);
    xSemaphoreTake(vsync_sem, portMAX_DELAY);
    // Czyszczenie i przygotowanie czystego bufora ekranu
    xSemaphoreTake(fb_mutex, portMAX_DELAY);
    memset(back_buffer, 0x00, 1024 * 600 * 2); // Czyszczenie na czarno (RGB565)

    // 0. PRZYCISK "GET TEMP" (NA SAMEJ GÓRZE: X=312, Y=85, W=400, H=100)
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 312, 85, 400, 100, 4, 0x06E0);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "GET TEMP", 358, 92, 80, 0x06E0);

    // =================================================================================
    // 1. PRZYCISK "START NTP" (WYŻEJ NA OSI Y)
    // =================================================================================
    // Pozycja: X=390, Y=300, Szerokość=500, Wysokość=120, grubość=4, kolor=zielony
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 312, 225, 400, 100, 4, 0x3DDF);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "START NTP", 344, 232, 80, 0x3DDF);

    // 4. To ZOSTAJE: Przycisk DONE (Ramka i napis) - użytkownik musi jakoś wyjść z menu
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 312, 450, 400, 90, 4, 0xFFFF);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "DONE", 400, 433, 120.0f, 0xFFFF);
    flush_screen_vsync();
    memcpy(back_buffer, (void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2);
    xSemaphoreGive(fb_mutex);

    // vTaskResume(hTaskTouch); // Budzimy dotyk dedykowany dla tego menu

    // 👇 WYBUDZENIE PRZED PĘTLĄ WHILE 👇
    vTaskResume(hTaskHardwareTouchDriver);

    bool exit_menu = false;
    while (!exit_menu)
    {
        // Sprawdzamy stan z niskopoziomowego wątku sprzętowego
        if (global_is_touched)
        {
            uint16_t tx = global_touchX;
            uint16_t ty = global_touchY;

            // --- DEBUGER DOTYKU NA EKRANIE ---
            // Czyścimy mały pasek pod cyframi w lewym rogu i rysujemy koordynaty
            // draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 7, 7, 300, 35, 0x0000);
            // show_debug_touch(tx, ty);

            // 1. HIT-BOX dla przycisku START NTP (Ramka Y=300, wysokość 120 -> ty od ok. 300 do 450)
            if (tx >= 312 && tx <= 712 && ty >= 225 && ty <= 325)
            {
                if (!is_ntp_sync_running())
                {
                    printf("[MENU NTP] -> Ręczne wywołanie synchronizacji czasu...\n");

                    // Czyścimy przestrzeń pomiędzy przyciskami (Y=450 do 580) na czarno pod status
                    draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 0, 330, 1024, 112, 0x0943);
                    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "SYNCHRONIZACJA...", 288, 350, 56.0f, 0xFE80);

                    // Wypychamy status na ekran przed uruchomieniem workera
                    flush_screen_vsync();
                    memcpy(back_buffer, (void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2);
                    start_ntp_sync_task_async(); // Start wątku sieciowego

                    // Czekamy na zakończenie pobierania czasu
                    while (is_ntp_sync_running())
                    {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }

                    if (ntp_read == true)
                    {
                        time_t now = 0;
                        struct tm timeinfo = {};
                        time(&now);
                        localtime_r(&now, &timeinfo);
                        rx8025t_time_t rtc_time;
                        rtc_time.second = timeinfo.tm_sec;
                        rtc_time.minute = timeinfo.tm_min;
                        rtc_time.hour = timeinfo.tm_hour;
                        rtc_time.day = timeinfo.tm_mday;
                        rtc_time.month = timeinfo.tm_mon + 1;
                        rtc_time.year = timeinfo.tm_year + 1900;
                        rtc_time.weekday = timeinfo.tm_wday;

                        char buf[64]; // Bufor na gotowy tekst (64 bajty w zupełności wystarczą)

                        snprintf(buf, sizeof(buf), "SUKCES NTP! %04d-%02d-%02d %02d:%02d:%02d",
                                 rtc_time.year, rtc_time.month, rtc_time.day,
                                 rtc_time.hour, rtc_time.minute, rtc_time.second);
                        draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 0, 330, 1024, 112, 0x1E);
                        draw_text(back_buffer, LCD_H_RES, LCD_V_RES, &buf[0], 25, 350, 66.0f, 0xFFFF);
                        flush_screen_vsync();
                        memcpy(back_buffer, (void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2);
                    }
                    else
                    {
                        draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 0, 330, 1024, 112, 0xF100);
                        // draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 100, 330, 800, 112, 0xF100);
                        draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "NIEPOWODZENIE--NTP !", 240, 350, 56.0f, 0xFFFF);
                        flush_screen_vsync();
                        memcpy(back_buffer, (void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2);
                    }
                }
            }

            // 0. HIT-BOX dla przycisku GET TEMP (X=312..712, Y=85..185)
            if (tx >= 312 && tx <= 712 && ty >= 85 && ty <= 185)
            {
                // GET_TEMP Obsługa kliknięcia przycisku GET TEMP, wywołanie funkcji pobierania temperatury oraz aktualizacja bufora ramki i ekranu LCD.
                if (!is_weather_fetch_running())
                {
                    if (!is_ntp_sync_running())
                    {
                        printf("[MENU NTP] -> Ręczne wywołanie synchronizacji czasu...\n");

                        // Czyścimy przestrzeń pomiędzy przyciskami (Y=450 do 580) na czarno pod status
                        draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 0, 330, 1024, 112, 0x2841);
                        draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "POBIERANIE TEMP...", 288, 350, 56.0f, 0xFE80);

                        // Wypychamy status na ekran przed uruchomieniem workera
                        flush_screen_vsync();
                        memcpy(back_buffer, (void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2);
                        float nowa_temp = -999.0f;
                        bool ok = fetch_weather_temperature(&nowa_temp);

                        if (ok && temp_read)
                        {
                            g_outdoor_temp = nowa_temp;
                            char buf[64];
                            snprintf(buf, sizeof(buf), "SUKCES GET TEMP ! %.1f", g_outdoor_temp);
                            draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 0, 330, 1024, 112, 0x06E0);
                            draw_text(back_buffer, LCD_H_RES, LCD_V_RES, &buf[0], 110, 350, 66.0f, 0xFFE0);

                            int end_x = 110 + (strlen(buf) * 31) + 6;
                            draw_text(back_buffer, LCD_H_RES, LCD_V_RES, ".", end_x + 4, 345, 40.0f, 0xFFE0);
                            draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "C", end_x + 17, 350, 66.0f, 0xFFE0);
                            flush_screen_vsync();
                            memcpy(back_buffer, (void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2);
                        }
                        else
                        {
                            draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 0, 330, 1024, 112, 0xF100);
                            // draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 100, 330, 800, 112, 0xF100);
                            draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "NIEPOWODZENIE GET TEMP !", 240, 350, 56.0f, 0xFFFF);
                            flush_screen_vsync();
                            memcpy(back_buffer, (void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2);
                        }
                    }
                }
            }

            // 2. HIT-BOX dla przycisku DONE (Ramka Y=600, wysokość 120 -> ty od ok. 600 do 750)
            if (tx >= 312 && tx <= 712 && ty >= 450 && ty <= 540)
            {
                printf("[MENU NTP] -> Kliknięto DONE. Powrót do zegara.\n");
                wait_for_release(); // Czekamy na puszczenie ekranu przed wyjściem z menu, aby uniknąć natychmiastowego wyzwolenia przycisku
                vTaskDelay(pdMS_TO_TICKS(150));
                exit_menu = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15)); // Częstotliwość odświeżania pętli
    }

    vTaskSuspend(hTaskHardwareTouchDriver);

    wait_for_release();             // Czekamy na puszczenie ekranu przed wejściem do menu, aby uniknąć natychmiastowego wyzwolenia przycisku
    vTaskDelay(pdMS_TO_TICKS(150)); // Dajemy czas na fizyczne przemieszczenie kontrolera i zdejmujemy blokadę czasu

    // Powrót do ekranu głównego
    // is_editing_time = false;
    old_seconds = 111; // Wymuszenie pełnego odświeżenia grafiki ekranu głównego
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void refresh_menu_self(void)
{
    xSemaphoreTake(fb_mutex, portMAX_DELAY);
    memset(back_buffer, 0x00, 1024 * 600 * 2);
    // Zamiast środkowego PAINT, wstaw:
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 56, 250, 400, 100, 4, 0xFFE0);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "PAINT", 164, 258, 80.0f, 0xFFE0);
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 568, 250, 400, 100, 4, 0xFFE0);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "SLIDE", 676, 258, 80.0f, 0xFFE0);

    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 56, 105, 400, 90, 4, 0x07E0);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "NTP", 200, 107, 80.0f, 0x07E0);
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 568, 105, 400, 90, 4, 0xF100);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "DEMO", 697, 107, 80.0f, 0xF100);
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 56, 405, 400, 90, 4, 0x07FF);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "WiFi", 185, 407, 80.0f, 0x07FF);
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 568, 405, 400, 90, 4, 0xFFFF);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "DONE", 695, 407, 80.0f, 0xFFFF);
    flush_screen_vsync();
    memcpy(back_buffer, (void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2);
    xSemaphoreGive(fb_mutex);
    wait_for_release();
}

void set_ntp_and_demo_menu(void)
{
    bool exit_menu = false;

    refresh_menu_self();

    global_is_touched = false;
    global_touchX = 0;
    global_touchY = 0;

    vTaskResume(hTaskHardwareTouchDriver);

    while (!exit_menu)
    {
        uint16_t tx = global_touchX, ty = global_touchY;

        if (global_is_touched)
        {
            // Obsluga przycisku NTP (lewa gorna cwiartka)
            if (tx >= 56 && tx <= 456 && ty >= 105 && ty <= 195)
            {
                set_ntp_menu();
                vTaskDelay(pdMS_TO_TICKS(30));
                vTaskResume(hTaskHardwareTouchDriver);
                vTaskDelay(pdMS_TO_TICKS(150));
                refresh_menu_self(); // Czyste wywołanie odświeżenia
            }

            // Obsluga przycisku DEMO (prawa gorna cwiartka)
            if (tx >= 568 && tx <= 968 && ty >= 105 && ty <= 195)
            {
                printf("[MENU NTP] -> Uruchamiam Vector Dots Demo...\n");
                vTaskDelay(pdMS_TO_TICKS(30));
                run_vector_dots_demo_loop();
                refresh_menu_self();
                wait_for_release();
                demo_exit_requested = false;
                global_is_touched = false;
                vTaskDelay(pdMS_TO_TICKS(30));
            }
            // PAINT (lewy środek: X=56..456, Y=250..350)
            if (tx >= 56 && tx <= 456 && ty >= 250 && ty <= 350)
            {
                printf("[MENU NTP/DEMO] -> Wybrano PAINT\n");
                run_paint_menu();
                exit_menu = true;
            }
            // SLIDE (prawy środek: X=568..968, Y=250..350)
            if (tx >= 568 && tx <= 968 && ty >= 250 && ty <= 350)
            {
                printf("[MENU NTP/DEMO] -> Wybrano SLIDE\n");
                set_slide_menu();
                refresh_menu_self();
                vTaskResume(hTaskHardwareTouchDriver);
            }

            // Obsluga przycisku DONE (wyjscie na dolnym pasku)
            if (tx >= 568 && tx <= 968 && ty >= 405 && ty <= 495)
            {
                printf("[MENU NTP] -> Kliknieto DONE. Powrot.\n");
                exit_menu = true;
            }
            // Obsluga przycisku WiFi (wyjscie na dolnym pasku)
            if (tx >= 56 && tx <= 456 && ty >= 405 && ty <= 495)
            {
                start_wifi_portal();

                xSemaphoreTake(fb_mutex, portMAX_DELAY);
                memset(back_buffer, 0x00, 1024 * 600 * 2);
                draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "AP: ESP32-CLOCK-SETUP", 80, 110, 60.0f, 0x07FF);
                draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "URL: http://192.168.4.1", 80, 200, 60.0f, 0x07E0);
                draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 568, 405, 400, 90, 4, 0xF100);
                draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "CANCEL", 650, 412, 80.0f, 0xF100);
                flush_screen_vsync();
                xSemaphoreGive(fb_mutex);
                wait_for_release();
                global_is_touched = false;
                vTaskResume(hTaskHardwareTouchDriver);

                while (is_wifi_portal_active())
                {
                    if (global_is_touched && global_touchX >= 568 && global_touchX <= 968 && global_touchY >= 405 && global_touchY <= 495)
                    {
                        stop_wifi_portal();
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
                refresh_menu_self();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    vTaskSuspend(hTaskHardwareTouchDriver);

    wait_for_release();
    vTaskDelay(pdMS_TO_TICKS(150));
    old_seconds = 111;
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void run_vector_dots_demo_loop(void)
{
    // NEW VECTOR DOTS DEMO
    wait_for_release();
    vTaskDelay(pdMS_TO_TICKS(200));
    start_vectordots_ball();
    vTaskDelay(pdMS_TO_TICKS(50));
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void flush_screen_vsync(void)
{
    uint8_t next_idx = 1 - active_fb_idx;
    esp_cache_msync((void *)fbs[next_idx], LCD_H_RES * LCD_V_RES * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)fbs[next_idx]);
    xSemaphoreTake(vsync_sem, portMAX_DELAY);
    active_fb_idx = next_idx;
    back_buffer = (uint16_t *)fbs[1 - active_fb_idx];
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void draw_brush_circle(uint16_t *buf, int cx, int cy, int r, uint16_t color)
{
    for (int dy = -r; dy <= r; dy++)
    {
        int py = cy + dy;
        if (py < 70 || py >= 600)
            continue;
        int dx = (int)sqrtf((float)(r * r - dy * dy));
        int x1 = cx - dx;
        int w = (dx * 2) + 1;
        if (x1 < 0)
        {
            w += x1;
            x1 = 0;
        }
        if (x1 + w > 1024)
        {
            w = 1024 - x1;
        }
        if (w > 0)
            draw_rectangle(buf, LCD_H_RES, LCD_V_RES, x1, py, w, 1, color);
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void draw_paint_toolbar(int sel_color_idx, int sel_size_idx)
{
    uint16_t colors[7] = {0xFFFF, 0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F, 0x0000};
    draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 0, 0, 1024, 66, 0x18E3);
    draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 0, 66, 1024, 3, 0x8410);

    for (int i = 0; i < 7; i++)
    {
        int bx = 8 + (i * 70);
        draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, bx, 6, 64, 54, colors[i]);
        uint16_t border = (i == sel_color_idx) ? 0xFFFF : 0x4208;
        int thick = (i == sel_color_idx) ? 4 : 2;
        draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, bx, 6, 64, 54, thick, border);
    }

    int radii[3] = {3, 7, 15};
    for (int s = 0; s < 3; s++)
    {
        int sx = 515 + (s * 74);
        uint16_t s_border = (s == sel_size_idx) ? 0x07FF : 0x4208;
        int s_thick = (s == sel_size_idx) ? 4 : 2;
        draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, sx, 6, 68, 54, s_thick, s_border);

        int r = radii[s];
        for (int dy = -r; dy <= r; dy++)
        {
            int dx = (int)sqrtf((float)(r * r - dy * dy));
            draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, sx + 34 - dx, 33 + dy, (dx * 2) + 1, 1, 0xFFFF);
        }
    }

    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 750, 6, 126, 54, 3, 0xFD20);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "CLR", 778, 12, 42.0f, 0xFD20);

    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 888, 6, 126, 54, 3, 0xF800);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "EXIT", 908, 12, 42.0f, 0xF800);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void run_paint_menu(void)
{
    uint16_t colors[7] = {0xFFFF, 0xF800, 0x07E0, 0x001F, 0xFFE0, 0xF81F, 0x0000};
    int radii[3] = {3, 7, 15};
    int sel_color = 3;
    int sel_size = 1;

    xSemaphoreTake(fb_mutex, portMAX_DELAY);
    draw_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 0, 0, 1024, 600, 0x0000);
    draw_paint_toolbar(sel_color, sel_size);
    flush_screen_vsync();
    memcpy(back_buffer, (void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2);
    xSemaphoreGive(fb_mutex);

    vTaskDelay(pdMS_TO_TICKS(250));

    bool exit_paint = false;
    bool is_drawing = false;
    int prev_x = -1;
    int prev_y = -1;
    uint16_t tx = 0, ty = 0;

    while (!exit_paint)
    {
        if (touch.getTouch(&tx, &ty))
        {
            int px = (int)tx;
            int py = (int)ty;

            if (py < 68)
            {
                is_drawing = false;
                bool toolbar_changed = false;

                for (int i = 0; i < 7; i++)
                {
                    int bx = 8 + (i * 70);
                    if (px >= bx && px <= bx + 64)
                    {
                        sel_color = i;
                        toolbar_changed = true;
                    }
                }
                for (int s = 0; s < 3; s++)
                {
                    int sx = 515 + (s * 74);
                    if (px >= sx && px <= sx + 68)
                    {
                        sel_size = s;
                        toolbar_changed = true;
                    }
                }
                if (px >= 750 && px <= 876)
                {
                    xSemaphoreTake(fb_mutex, portMAX_DELAY);
                    memset((uint8_t *)fbs[0] + (69 * 1024 * 2), 0x00, 531 * 1024 * 2);
                    memset((uint8_t *)fbs[1] + (69 * 1024 * 2), 0x00, 531 * 1024 * 2);

                    toolbar_changed = true;
                    xSemaphoreGive(fb_mutex);
                }
                if (px >= 888 && px <= 1014)
                {
                    exit_paint = true;
                }

                if (toolbar_changed && !exit_paint)
                {
                    xSemaphoreTake(fb_mutex, portMAX_DELAY);
                    draw_paint_toolbar(sel_color, sel_size);
                    flush_screen_vsync();
                    memcpy(back_buffer, (void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2);
                    xSemaphoreGive(fb_mutex);
                    vTaskDelay(pdMS_TO_TICKS(150));
                }
            }
            else
            {
                xSemaphoreTake(fb_mutex, portMAX_DELAY);
                int r = radii[sel_size];
                uint16_t col = colors[sel_color];

                if (!is_drawing || prev_x < 0)
                {
                    draw_brush_circle((uint16_t *)fbs[0], px, py, r, col);
                    draw_brush_circle((uint16_t *)fbs[1], px, py, r, col);
                }
                else
                {
                    int dx = px - prev_x;
                    int dy = py - prev_y;
                    int steps = (abs(dx) > abs(dy)) ? abs(dx) : abs(dy);
                    if (steps == 0)
                        steps = 1;
                    for (int st = 0; st <= steps; st++)
                    {
                        int ix = prev_x + (dx * st) / steps;
                        int iy = prev_y + (dy * st) / steps;
                        draw_brush_circle((uint16_t *)fbs[0], ix, iy, r, col);
                        draw_brush_circle((uint16_t *)fbs[1], ix, iy, r, col);
                    }
                }
                prev_x = px;
                prev_y = py;
                is_drawing = true;
                // esp_cache_msync((void *)fbs[0], LCD_H_RES * LCD_V_RES * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                // esp_cache_msync((void *)fbs[1], LCD_H_RES * LCD_V_RES * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                esp_cache_msync((void *)fbs[active_fb_idx], LCD_H_RES * LCD_V_RES * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                xSemaphoreGive(fb_mutex);
            }
        }
        else
        {
            is_drawing = false;
            prev_x = -1;
            prev_y = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    old_seconds = 111;
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_max_briggtness(void)
{
    set_brightness(100);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void restore_brightness(void)
{
    uint32_t duty = 1023 * BRIGHTNESS / 100;
    if (duty > 1023)
    {
        duty = 1023;
    }
    ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, duty);
    ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_brightness(uint8_t dana)
{
    uint32_t duty = 1023 * dana / 100;
    if (duty > 1023)
    {
        duty = 1023;
    }
    ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, duty);
    ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void save_slide_settings_to_NVS(void)
{
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK)
    {
        uint8_t en = g_slide_enabled ? 1 : 0, old_v = 255;
        bool dirty = false;
        if (nvs_get_u8(my_handle, "slide_en", &old_v) != ESP_OK || old_v != en)
        {
            nvs_set_u8(my_handle, "slide_en", en);
            dirty = true;
        }
        if (nvs_get_u8(my_handle, "slide_int", &old_v) != ESP_OK || old_v != g_slide_interval)
        {
            nvs_set_u8(my_handle, "slide_int", g_slide_interval);
            dirty = true;
        }
        if (nvs_get_u8(my_handle, "slide_cnt", &old_v) != ESP_OK || old_v != g_slide_photo_count)
        {
            nvs_set_u8(my_handle, "slide_cnt", g_slide_photo_count);
            dirty = true;
        }
        if (dirty)
            nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void load_slide_settings_from_NVS(void)
{
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK)
    {
        uint8_t en = 1, inter = 5, cnt = 10;
        if (nvs_get_u8(my_handle, "slide_en", &en) == ESP_OK)
            g_slide_enabled = (en != 0);
        if (nvs_get_u8(my_handle, "slide_int", &inter) == ESP_OK)
            g_slide_interval = inter;
        if (nvs_get_u8(my_handle, "slide_cnt", &cnt) == ESP_OK)
            g_slide_photo_count = cnt;
        nvs_close(my_handle);
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void draw_slide_menu_screen(void)
{
    char buf[32];
    xSemaphoreTake(fb_mutex, portMAX_DELAY);
    safe_clear_screen(back_buffer, 0x0000);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "SLIDE SHOW SETTINGS", 248, 15, 50.0f, 0x07FF);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "AUTO SLIDE:", 80, 94, 42.0f, 0xFFFF);
    uint16_t en_color = g_slide_enabled ? 0x07E0 : 0xF800;
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 600, 82, 320, 68, 4, en_color);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, g_slide_enabled ? "YES" : "NO", g_slide_enabled ? 716 : 732, 94, 45.0f, en_color);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "PHOTOS COUNT:", 80, 198, 42.0f, 0xFFFF);
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 600, 187, 80, 68, 4, 0xFFFF);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "-", 630, 198, 50.0f, 0xFFFF);
    snprintf(buf, sizeof(buf), "%d", g_slide_photo_count);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, buf, (g_slide_photo_count >= 10) ? 732 : 746, 198, 50.0f, 0xFFE0);
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 840, 187, 80, 68, 4, 0xFFFF);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "+", 868, 198, 50.0f, 0xFFFF);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "INTERVAL:", 80, 303, 42.0f, 0xFFFF);
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 600, 292, 320, 68, 4, 0x07FF);
    snprintf(buf, sizeof(buf), "%d MIN", g_slide_interval);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, buf, (g_slide_interval >= 10) ? 692 : 708, 303, 45.0f, 0x07FF);
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 80, 435, 384, 82, 4, 0xFFE0);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "SHOW PHOTOS", 140, 454, 45.0f, 0xFFE0);
    draw_hollow_rectangle(back_buffer, LCD_H_RES, LCD_V_RES, 560, 435, 360, 82, 4, 0x07E0);
    draw_text(back_buffer, LCD_H_RES, LCD_V_RES, "DONE", 680, 454, 50.0f, 0x07E0);
    flush_screen_vsync();
    xSemaphoreGive(fb_mutex);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_slide_menu(void)
{
    draw_slide_menu_screen();
    wait_for_release();
    vTaskDelay(pdMS_TO_TICKS(50));
    while (1)
    {
        if (global_is_touched)
        {
            uint16_t tx = global_touchX, ty = global_touchY;
            // Przycisk AUTO SLIDE
            if (tx >= 600 && tx <= 920 && ty >= 82 && ty <= 150)
            {
                g_slide_enabled = !g_slide_enabled;
                draw_slide_menu_screen();
            }
            // Przycisk - (zmniejszenie licznika)
            else if (tx >= 600 && tx <= 680 && ty >= 187 && ty <= 255)
            {
                if (g_slide_photo_count > 1)
                    g_slide_photo_count--;
                draw_slide_menu_screen();
            }
            // Przycisk + (zwiększenie licznika)
            else if (tx >= 840 && tx <= 920 && ty >= 187 && ty <= 255)
            {
                if (g_slide_photo_count < 99)
                    g_slide_photo_count++;
                draw_slide_menu_screen();
            }
            else if (tx >= 600 && tx <= 920 && ty >= 292 && ty <= 360)
            {
                int idx = 0;
                for (int i = 0; i < SLIDE_INTERVALS_COUNT; i++)
                    if (SLIDE_INTERVALS[i] == g_slide_interval)
                    {
                        idx = (i + 1) % SLIDE_INTERVALS_COUNT;
                        break;
                    }
                g_slide_interval = SLIDE_INTERVALS[idx];
                draw_slide_menu_screen();
            }
            else if (tx >= 80 && tx <= 464 && ty >= 435 && ty <= 517)
            {
                set_max_briggtness();
                show_all_photos_manual();
                apply_scheduled_brightness(dane_czasu.tm_hour, true);
            }
            else if (tx >= 560 && tx <= 920 && ty >= 435 && ty <= 517)
            {
                save_slide_settings_to_NVS();
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    wait_for_release();
    vTaskDelay(pdMS_TO_TICKS(150));
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void show_all_photos_manual(void)
{
    // uint8_t target_br = (dane_czasu.tm_hour >= 20 || dane_czasu.tm_hour < 6) ? BRIGHTNESS_NIGHT : BRIGHTNESS;
    //  set_brightness(target_br);
    for (int idx = 1; idx <= g_slide_photo_count; idx++)
    {
        char path[32];
        snprintf(path, sizeof(path), "/spiffs/FOTO%d.jpg", idx);
        struct stat st;
        if (stat(path, &st) != 0)
            break;
        wyswietl_jpeg_ze_spiffs(path);
        wait_for_release();
        while (!global_is_touched)
            vTaskDelay(pdMS_TO_TICKS(20));
    }
    wait_for_release();
    draw_slide_menu_screen();
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
