#include "esp_program.h"
#include "hw_def.h"
#include "esp_loader_stubs.h"
#include <string.h>

static esp_loader_t loader;
static uint8_t      payload[4096];

const char *espProgramError(esp_loader_error_t code)
{
  switch (code)
  {
    case ESP_LOADER_SUCCESS:
      return "OK";
    case ESP_LOADER_ERROR_TIMEOUT:
      return "UART timeout";
    case ESP_LOADER_ERROR_INVALID_MD5:
      return "MD5 mismatch";
    case ESP_LOADER_ERROR_IMAGE_SIZE:
      return "Flash too small";
    case ESP_LOADER_ERROR_INVALID_TARGET:
      return "Wrong ESP chip";
    case ESP_LOADER_ERROR_INVALID_RESPONSE:
      return "UART response";
    case ESP_LOADER_ERROR_UNSUPPORTED_CHIP:
      return "Unsupported chip";
    case ESP_LOADER_ERROR_UNSUPPORTED_FUNC:
      return "Unsupported mode";
    case ESP_LOADER_ERROR_INVALID_PARAM:
      return "Invalid parameter";
    default:
      return "ESP loader error";
  }
}

static target_chip_t espProgramTarget(esp_project_chip_t chip)
{
  switch (chip)
  {
    case ESP_PROJECT_32:
      return ESP32_CHIP;
    case ESP_PROJECT_S3:
      return ESP32S3_CHIP;
    case ESP_PROJECT_C3:
      return ESP32C3_CHIP;
    default:
      return ESP_UNKNOWN_CHIP;
  }
}

static bool espProgramIdentify(esp_project_chip_t chip, esp_program_result_t *result);

typedef struct
{
  esp_project_chip_t    chip;
  esp_program_result_t *result;
} esp_stub_context_t;

static const esp_stub_t *espProgramStub(esp_loader_t *connection, target_chip_t chip, void *context)
{
  (void)connection;
  esp_stub_context_t *request = context;
  request->result->detected   = chip;
  /* Validate in ROM, before executing any uploaded code. */
  if (!espProgramIdentify(request->chip, request->result))
  {
    return NULL;
  }
  request->result->stage = "ESP LOAD STUB";
  switch (chip)
  {
    case ESP32_CHIP:
      return &esp_stub_esp32;
    case ESP32S3_CHIP:
      return &esp_stub_esp32s3;
    case ESP32C3_CHIP:
      return &esp_stub_esp32c3;
    default:
      return NULL;
  }
}

static bool espProgramConnect(esp_program_result_t *result, bool use_stub, esp_project_chip_t chip)
{
  result->stub  = false;
  result->stage = "ESP CONNECT";
  result->code  = esp_loader_init_serial(&loader, espPortGet());
  if (result->code != ESP_LOADER_SUCCESS)
  {
    return false;
  }
  esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
  if (use_stub)
  {
    esp_stub_context_t context = {chip, result};
    esp_loader_error_t code    = esp_loader_connect_with_stub_provider(&loader, &args, espProgramStub, &context);
    if (result->code == ESP_LOADER_SUCCESS)
    {
      result->code = code;
    }
    result->stub = result->code == ESP_LOADER_SUCCESS;
  }
  else
  {
    result->code = esp_loader_connect(&loader, &args);
  }
  if (result->code == ESP_LOADER_SUCCESS)
  {
    result->detected = esp_loader_get_target(&loader);
  }
  return result->code == ESP_LOADER_SUCCESS && !result->error;
}

static bool espProgramFinish(esp_program_result_t *result, uint32_t start)
{
  espPortStats(&result->port);
  espPortClose();
  result->elapsed_ms = millis() - start;
  if (!result->error && result->code != ESP_LOADER_SUCCESS)
  {
    result->error = espProgramError(result->code);
  }
  return !result->error;
}

bool espProgramProbe(esp_program_result_t *result)
{
  memset(result, 0, sizeof(*result));
  result->detected = ESP_UNKNOWN_CHIP;
  result->stage    = "ESP PREFLIGHT";
  uint32_t start   = millis();
  if (espProgramConnect(result, false, ESP_PROJECT_32))
  {
    result->stage = "ESP FLASH ID";
    result->code  = esp_loader_flash_detect_size(&loader, &result->flash_size);
  }
  /* Leave ROM downloader active. Probe resets target, but never erases/writes flash. */
  return espProgramFinish(result, start);
}

static bool espProgramProgress(const esp_program_input_t *input, esp_program_result_t *result, const char *stage,
                               uint32_t done, uint32_t total)
{
  result->stage = stage;
  if (!input->progress(input->context, stage, done, total))
  {
    result->error = "Stopped";
    return false;
  }
  return true;
}

static bool espProgramIdentify(esp_project_chip_t chip, esp_program_result_t *result)
{
  result->stage = "ESP CHIP CHECK";
  if (result->detected != espProgramTarget(chip))
  {
    result->code = ESP_LOADER_ERROR_INVALID_TARGET;
    return false;
  }
  if (!result->stub && result->detected != ESP32_CHIP)
  {
    esp_loader_target_security_info_t security = {0};
    result->stage                              = "ESP SECURITY";
    result->code                               = esp_loader_get_security_info(&loader, &security);
    if (result->code != ESP_LOADER_SUCCESS)
    {
      return false;
    }
    if (security.secure_boot_enabled || security.flash_encryption_enabled || security.secure_download_mode_enabled)
    {
      result->error = "Protected ESP";
      return false;
    }
  }
  result->stage = "ESP FLASH ID";
  result->code  = esp_loader_flash_detect_size(&loader, &result->flash_size);
  if (result->code != ESP_LOADER_SUCCESS)
  {
    return false;
  }
  if (result->flash_size < 0x100000 || result->flash_size > ESP_PROJECT_LIMIT ||
      (result->flash_size & (result->flash_size - 1)))
  {
    result->code = ESP_LOADER_ERROR_IMAGE_SIZE;
    return false;
  }
  return true;
}

static bool espProgramBaudValid(uint32_t baud)
{
  return baud == 0 || baud == 115200 || baud == 230400 || baud == 460800 || baud == 921600;
}

/* Only negotiate before any erase/write. A failed candidate reconnects at 115200. */
static bool espProgramTune(esp_project_chip_t chip, uint32_t ceiling, const esp_program_input_t *input,
                           esp_program_result_t *result)
{
  static const uint32_t rates[]  = {921600, 460800, 230400, 115200};
  uint32_t              start    = millis();
  uint32_t              baseline = 0;
  result->stage                  = "ESP LINK TEST";
  result->code                   = esp_loader_read_register(&loader, 0x40001000, &baseline);
  if (result->code != ESP_LOADER_SUCCESS)
  {
    return false;
  }
  for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
  {
    if (ceiling && rates[i] > ceiling)
    {
      continue;
    }
    if (!espProgramProgress(input, result, "ESP LINK TEST", 0, 1))
    {
      return false;
    }
    result->code = rates[i] == 115200 ? ESP_LOADER_SUCCESS : esp_loader_change_transmission_rate(&loader, rates[i]);
    for (unsigned sample = 0; sample < 16 && result->code == ESP_LOADER_SUCCESS; sample++)
    {
      uint32_t value = 0;
      result->code   = esp_loader_read_register(&loader, 0x40001000, &value);
      if (result->code == ESP_LOADER_SUCCESS && value != baseline)
      {
        result->code = ESP_LOADER_ERROR_INVALID_RESPONSE;
      }
    }
    if (result->code == ESP_LOADER_SUCCESS)
    {
      /* Exercise the ROM SPI-register commands at the selected speed, too. */
      if (!espProgramIdentify(chip, result))
      {
        if (result->error || result->code == ESP_LOADER_ERROR_INVALID_TARGET ||
            result->code == ESP_LOADER_ERROR_IMAGE_SIZE)
        {
          return false;
        }
      }
      else
      {
        result->link_test_ms = millis() - start;
        return true;
      }
    }
    if (rates[i] == 115200)
    {
      break;
    }
    result->rate_fallbacks++;
    espPortClose();
    if (!espProgramConnect(result, true, chip) || !espProgramIdentify(chip, result))
    {
      return false;
    }
  }
  result->link_test_ms = millis() - start;
  return false;
}

bool espProgramRun(const esp_project_t *project, const esp_program_input_t *input, esp_program_result_t *result)
{
  memset(result, 0, sizeof(*result));
  result->detected = ESP_UNKNOWN_CHIP;
  result->stage    = "ESP PREFLIGHT";
  uint32_t start   = millis();
  if (!project || !input || !input->read || !input->progress || !project->count ||
      project->count > ESP_PROJECT_IMAGES || !espProgramBaudValid(project->baud) ||
      espProgramTarget(project->chip) == ESP_UNKNOWN_CHIP)
  {
    result->error = "Invalid ESP input";
    return false;
  }
  if (!espProgramProgress(input, result, "ESP CONNECT", 0, project->total) ||
      !espProgramConnect(result, true, project->chip))
  {
    return espProgramFinish(result, start);
  }
  if (!espProgramIdentify(project->chip, result) || !espProgramTune(project->chip, project->baud, input, result))
  {
    return espProgramFinish(result, start);
  }
  if (result->flash_size < project->flash_size)
  {
    result->code = ESP_LOADER_ERROR_IMAGE_SIZE;
    return espProgramFinish(result, start);
  }
  /* Validate EVERY range against the actual chip before the first erase. */
  for (unsigned i = 0; i < project->count; i++)
  {
    const esp_project_image_t *image = &project->images[i];
    uint32_t                   size  = (image->size + 4095) & ~4095U;
    if (image->address >= result->flash_size || size > result->flash_size - image->address)
    {
      result->code = ESP_LOADER_ERROR_IMAGE_SIZE;
      return espProgramFinish(result, start);
    }
  }
  for (unsigned i = 0; i < project->count; i++)
  {
    const esp_project_image_t *image = &project->images[i];
    result->address                  = image->address;
    if (!espProgramProgress(input, result, "ESP ERASE", result->written, project->total))
    {
      return espProgramFinish(result, start);
    }
    esp_loader_flash_cfg_t config = {
      .offset      = image->address,
      .image_size  = (image->size + 3) & ~3U,
      .block_size  = sizeof(payload),
      .skip_verify = false,
    };
    result->code = esp_loader_flash_start(&loader, &config);
    if (result->code != ESP_LOADER_SUCCESS)
    {
      return espProgramFinish(result, start);
    }
    uint32_t crc = UINT32_MAX;
    for (uint32_t offset = 0; offset < image->size;)
    {
      result->address = image->address + offset;
      if (!espProgramProgress(input, result, "ESP WRITE", result->written, project->total))
      {
        return espProgramFinish(result, start);
      }
      uint32_t n = image->size - offset;
      if (n > sizeof(payload))
      {
        n = sizeof(payload);
      }
      memset(payload, 0xFF, sizeof(payload));
      if (!input->read(input->context, i, offset, payload, n))
      {
        result->error = "ESP SD read";
        return espProgramFinish(result, start);
      }
      for (uint32_t b = 0; b < n; b++)
      {
        crc ^= payload[b];
        for (unsigned bit = 0; bit < 8; bit++)
        {
          crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1)));
        }
      }
      result->code = esp_loader_flash_write(&loader, &config, payload, (n + 3) & ~3U);
      if (result->code != ESP_LOADER_SUCCESS)
      {
        return espProgramFinish(result, start);
      }
      offset          += n;
      result->written += n;
    }
    if (~crc != image->crc32)
    {
      result->error = "SD changed / CRC";
      return espProgramFinish(result, start);
    }
    if (!espProgramProgress(input, result, "ESP VERIFY MD5", result->verified, project->total))
    {
      return espProgramFinish(result, start);
    }
    result->code = esp_loader_flash_finish(&loader, &config);
    if (result->code != ESP_LOADER_SUCCESS)
    {
      return espProgramFinish(result, start);
    }
    result->verified += image->size;
  }
  if (!espProgramProgress(input, result, "ESP RESTART", result->verified, project->total))
  {
    return espProgramFinish(result, start);
  }
  esp_loader_reset_target(&loader);
  result->started = true;
  result->stage   = "DONE";
  return espProgramFinish(result, start);
}

/* Full flash erase, including partition table, OTA metadata, NVS and filesystems. */
bool espProgramErase(esp_project_chip_t chip, const esp_program_input_t *input, esp_program_result_t *result)
{
  static const uint8_t blank_md5[] = "2fdd6851b32ae931637d4845c037b550"; /* 1 MiB of FF */
  const uint32_t       block_size  = 0x100000;
  memset(result, 0, sizeof(*result));
  result->detected = ESP_UNKNOWN_CHIP;
  result->stage    = "ESP PREFLIGHT";
  uint32_t start   = millis();
  if (!input || !input->progress || espProgramTarget(chip) == ESP_UNKNOWN_CHIP)
  {
    result->error = "Invalid ESP input";
    return false;
  }
  if (!espProgramProgress(input, result, "ESP CONNECT", 0, 1) || !espProgramConnect(result, true, chip) ||
      !espProgramIdentify(chip, result) || !espProgramTune(chip, 0, input, result))
  {
    return espProgramFinish(result, start);
  }
  if (!espProgramProgress(input, result, "ESP CHIP ERASE", 0, result->flash_size))
  {
    return espProgramFinish(result, start);
  }
  result->code = esp_loader_flash_erase(&loader);
  if (result->code != ESP_LOADER_SUCCESS)
  {
    return espProgramFinish(result, start);
  }
  for (uint32_t address = 0; address < result->flash_size; address += block_size)
  {
    result->address = address;
    if (!espProgramProgress(input, result, "ESP BLANK CHECK", address, result->flash_size))
    {
      return espProgramFinish(result, start);
    }
    result->code = esp_loader_flash_verify_known_md5(&loader, address, block_size, blank_md5);
    if (result->code != ESP_LOADER_SUCCESS)
    {
      return espProgramFinish(result, start);
    }
    result->verified += block_size;
  }
  result->erase_complete = true;
  result->stage          = "DONE";
  /* Leave the blank target in the RAM stub downloader. */
  return espProgramFinish(result, start);
}
