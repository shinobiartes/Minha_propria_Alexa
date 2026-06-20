#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <FluxGarage_RoboEyes.h> 
#undef N
#undef NE
#undef E
#undef SE
#undef S
#undef SW
#undef W
#undef NW

#include "Audio.h" 
#include <driver/i2s.h> 
#include "mbedtls/base64.h" 

// ========================================================
// 🔐 CREDENCIAIS E SUAS 6 CHAVES DO GEMINI
// ========================================================
const char* WIFI_SSID     = "SUA REDE";
const char* WIFI_PASSWORD = "SUA SENHA";

const int TOTAL_APIS = 2; 
int api_atual = 0;
const char* LISTA_APIS[TOTAL_APIS] = {
  "SUA API", 
   "SUA API", 
};

#define SCREEN_WIDTH 128 
#define SCREEN_HEIGHT 64 
#define OLED_RESET     -1 
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
RoboEyes<Adafruit_SSD1306> roboEyes(display);

// Pinos
#define AMP_BCLK   12 
#define AMP_LRC    13 
#define AMP_DOUT   14 
#define MIC_WS    5  
#define MIC_SCK   6  
#define MIC_SD    4  
#define TRIG_PIN  17
#define ECHO_PIN  18
#define DISTANCIA_GATILHO 50 
#define PINO_RELE 21

Audio *audio = nullptr; 

#define TEMPO_GRAVACAO_SEGUNDOS 6
#define SAMPLE_RATE 16000
uint32_t tamanho_dados_wav = TEMPO_GRAVACAO_SEGUNDOS * SAMPLE_RATE * 2; 
uint32_t tamanho_total_wav = tamanho_dados_wav + 44;
uint8_t *buffer_gravacao = nullptr; 

enum EstadoRafaela {
  ESPERANDO,
  SAUDACAO_INICIAL,
  ESCUTANDO_MIC,
  PROCESSANDO_IA,
  CHECANDO_PRESENCA,
  DESPEDIDA,
  INICIAR_MUSICA,
  TOCANDO_MUSICA
};
EstadoRafaela estado_atual = ESPERANDO;

void escreverStatusOLED(String texto) {
  display.fillRect(0, 54, 128, 10, SSD1306_BLACK); 
  display.setCursor(0, 54); 
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.print(texto);
  display.display();
}

float lerDistanciaSensor() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  long duracao = pulseIn(ECHO_PIN, HIGH, 5000); 
  if (duracao == 0) return 999.0; 
  return duracao * 0.034 / 2;
}

void conectarWiFi() {
  Serial.println("[WIFI] A ligar...");
  escreverStatusOLED("A ligar WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 20) { delay(500); tentativas++; Serial.print("."); }
  if(WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Ligado!");
    escreverStatusOLED("WiFi OK!");
  }
  delay(1000);
}

void gerarCabecalhoWAV(uint8_t* cabecalho, uint32_t tamanhoDadosAudio) {
  uint32_t tamanhoArquivo = tamanhoDadosAudio + 36;
  uint32_t byteRate = SAMPLE_RATE * 2; 
  uint8_t header[44] = {
    'R', 'I', 'F', 'F',
    (uint8_t)(tamanhoArquivo & 0xFF), (uint8_t)((tamanhoArquivo >> 8) & 0xFF), (uint8_t)((tamanhoArquivo >> 16) & 0xFF), (uint8_t)((tamanhoArquivo >> 24) & 0xFF),
    'W', 'A', 'V', 'E',
    'f', 'm', 't', ' ',
    16, 0, 0, 0, 1, 0, 1, 0, 
    (uint8_t)(SAMPLE_RATE & 0xFF), (uint8_t)((SAMPLE_RATE >> 8) & 0xFF), (uint8_t)((SAMPLE_RATE >> 16) & 0xFF), (uint8_t)((SAMPLE_RATE >> 24) & 0xFF),
    (uint8_t)(byteRate & 0xFF), (uint8_t)((byteRate >> 8) & 0xFF), (uint8_t)((byteRate >> 16) & 0xFF), (uint8_t)((byteRate >> 24) & 0xFF),
    2, 0, 16, 0, 
    'd', 'a', 't', 'a',
    (uint8_t)(tamanhoDadosAudio & 0xFF), (uint8_t)((tamanhoDadosAudio >> 8) & 0xFF), (uint8_t)((tamanhoDadosAudio >> 16) & 0xFF), (uint8_t)((tamanhoDadosAudio >> 24) & 0xFF)
  };
  memcpy(cabecalho, header, 44);
}

void ligarMicrofone() {
  i2s_config_t mic_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, 
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4, .dma_buf_len = 1024, .use_apll = false
  };
  i2s_driver_install(I2S_NUM_1, &mic_config, 0, NULL);
  i2s_pin_config_t mic_pins = { .bck_io_num = MIC_SCK, .ws_io_num = MIC_WS, .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = MIC_SD };
  i2s_set_pin(I2S_NUM_1, &mic_pins);
  i2s_start(I2S_NUM_1);
}

void desligarMicrofone() {
  i2s_stop(I2S_NUM_1);
  i2s_driver_uninstall(I2S_NUM_1);
}

void falarTextoRafaela(String textoParaFalar) {
  audio = new Audio();
  audio->setPinout(AMP_BCLK, AMP_LRC, AMP_DOUT);
  
  audio->setVolume(18); 
  delay(50); 
  
  Serial.println("[RAFAELA]: " + textoParaFalar);
  escreverStatusOLED("A falar...");
  
  audio->connecttospeech(textoParaFalar.c_str(), "pt-BR");
  while(audio->isRunning()) { audio->loop(); roboEyes.update(); }
  audio->stopSong(); 
  delete audio;
  audio = nullptr;
}

String obterRespostaGeminiComAudio(uint8_t* wav_data, size_t wav_size) {
  Serial.println("[IA] A enviar para o Gemini...");
  
  size_t b64_len = 0;
  mbedtls_base64_encode(NULL, 0, &b64_len, wav_data, wav_size);
  char* b64_buf = (char*)ps_malloc(b64_len + 1);
  if (!b64_buf) return "Erro de memória.";
  
  mbedtls_base64_encode((unsigned char*)b64_buf, b64_len, &b64_len, wav_data, wav_size);
  b64_buf[b64_len] = '\0';

  String prefix = "{\"contents\":[{\"parts\":[{\"text\":\"Você é a robô Rafaela. Ouça o chefe Robson e responda curto. REGRA SECRETA: Se pedir para ligar luz, adicione a tag [RELE_ON]. Se desligar luz, [RELE_OFF]. Se pedir para tocar música, rádio ou som, responda com entusiasmo e adicione a tag [TOCA_MUSICA]. Não leia as tags em voz alta.\"},{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"";
  String suffix = "\"}}]}]}";

  size_t payload_len = prefix.length() + b64_len + suffix.length() + 1;
  char* payload_buf = (char*)ps_malloc(payload_len);
  if (!payload_buf) { free(b64_buf); return "Erro de memória."; }

  strcpy(payload_buf, prefix.c_str());
  strcat(payload_buf, b64_buf);
  strcat(payload_buf, suffix.c_str());
  free(b64_buf); 

  String resposta_final = "Falha de comunicação.";

  for (int tentativa = 0; tentativa < TOTAL_APIS; tentativa++) {
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();

    String url = "https://generativelanguage.googleapis.com/v1/models/gemini-2.5-flash:generateContent?key=" + String(LISTA_APIS[api_atual]);
    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");

    int httpResponseCode = http.POST((uint8_t*)payload_buf, strlen(payload_buf));
    String response = http.getString();
    http.end();

    if (httpResponseCode == 200) {
      int pos = response.indexOf("\"text\": \"");
      if (pos != -1) {
        int fim = response.indexOf("\"", pos + 9);
        String textoLimpo = response.substring(pos + 9, fim);
        textoLimpo.replace("\\n", "");
        textoLimpo.replace("\"", "");
        resposta_final = textoLimpo;
        break; 
      }
    }
    api_atual = (api_atual + 1) % TOTAL_APIS;
    delay(200); 
  }
  free(payload_buf); 
  return resposta_final;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  pinMode(PINO_RELE, OUTPUT);
  // 🚀 Começa em HIGH para o relé de lógica invertida iniciar desligado
  digitalWrite(PINO_RELE, HIGH); 

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  Wire.begin(41, 1);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { for(;;); }
  display.clearDisplay();
  display.display();

  roboEyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 100);
  roboEyes.setAutoblinker(ON, 3, 2); 
  roboEyes.setIdleMode(ON); 
  roboEyes.setWidth(24, 24);  
  roboEyes.setHeight(28, 28); 
  roboEyes.setSpacebetween(12);
  roboEyes.setMood(DEFAULT);

  conectarWiFi();
  falarTextoRafaela("Sistema ativado.");
}

void loop() {
  roboEyes.update();
  float distancia_atual = lerDistanciaSensor();

  switch (estado_atual) {
    case ESPERANDO: {
      escreverStatusOLED("A procurar...");
      if (distancia_atual > 0 && distancia_atual < DISTANCIA_GATILHO) {
        estado_atual = SAUDACAO_INICIAL;
      }
      break;
    }

    case SAUDACAO_INICIAL: {
      roboEyes.setMood(HAPPY); 
      falarTextoRafaela("E aí, o que manda chefe?");
      estado_atual = ESCUTANDO_MIC;
      break;
    }

    case ESCUTANDO_MIC: {
      escreverStatusOLED("A ouvir...");
      roboEyes.setMood(DEFAULT);
      buffer_gravacao = (uint8_t*)ps_malloc(tamanho_total_wav);
      
      gerarCabecalhoWAV(buffer_gravacao, tamanho_dados_wav);
      ligarMicrofone();
      size_t bytes_lidos = 0;
      uint32_t bytes_gravados = 0;
      const size_t TAMANHO_PEDACO = 1024; 
      
      while (bytes_gravados < tamanho_dados_wav) {
        roboEyes.update();
        size_t bytes_a_ler = tamanho_dados_wav - bytes_gravados;
        if (bytes_a_ler > TAMANHO_PEDACO) bytes_a_ler = TAMANHO_PEDACO;
        i2s_read(I2S_NUM_1, buffer_gravacao + 44 + bytes_gravados, bytes_a_ler, &bytes_lidos, portMAX_DELAY);
        bytes_gravados += bytes_lidos;
        delay(1); 
      }
      desligarMicrofone();
      estado_atual = PROCESSANDO_IA;
      break;
    }

    case PROCESSANDO_IA: {
      escreverStatusOLED("A pensar...");
      roboEyes.setMood(TIRED);
      String resposta = obterRespostaGeminiComAudio(buffer_gravacao, tamanho_total_wav);
      free(buffer_gravacao);
      buffer_gravacao = nullptr;

      bool vai_tocar_musica = false;

      // 🚀 INVERSÃO DA LÓGICA DO RELÉ AQUI
      if (resposta.indexOf("[RELE_ON]") != -1) {
        digitalWrite(PINO_RELE, LOW); // Envia 0V para o relé atracar e ligar a lâmpada
        resposta.replace("[RELE_ON]", ""); 
      }
      else if (resposta.indexOf("[RELE_OFF]") != -1) {
        digitalWrite(PINO_RELE, HIGH); // Envia 3.3V para o relé desarmar e desligar a lâmpada
        resposta.replace("[RELE_OFF]", ""); 
      }
      else if (resposta.indexOf("[TOCA_MUSICA]") != -1) {
        vai_tocar_musica = true;
        resposta.replace("[TOCA_MUSICA]", "");
      }
      
      roboEyes.setMood(HAPPY);
      falarTextoRafaela(resposta);

      if (vai_tocar_musica) {
        estado_atual = INICIAR_MUSICA;
      } else {
        estado_atual = CHECANDO_PRESENCA;
      }
      break;
    }

    case INICIAR_MUSICA: {
      escreverStatusOLED("Música On!");
      roboEyes.setMood(HAPPY);
      
      audio = new Audio();
      audio->setPinout(AMP_BCLK, AMP_LRC, AMP_DOUT);
      audio->setVolume(10); 
      
      audio->connecttohost("https://github.com/shinobiartes/Utilitarios/raw/refs/heads/main/Maw_of_the_Engine.mp3"); 
      
      estado_atual = TOCANDO_MUSICA;
      break;
    }

    case TOCANDO_MUSICA: {
      if (audio->isRunning()) {
        audio->loop(); 
        roboEyes.update(); 
        
        static unsigned long ultimo_teste_sensor = 0;
        if (millis() - ultimo_teste_sensor > 100) {
          float dist = lerDistanciaSensor();
          if (dist > 0 && dist < DISTANCIA_GATILHO) {
            Serial.println("[SISTEMA] Música interrompida.");
            audio->stopSong();
            delete audio;
            audio = nullptr;
            
            falarTextoRafaela("Música parada chefe. Mais alguma coisa?");
            estado_atual = ESCUTANDO_MIC; 
          }
          ultimo_teste_sensor = millis();
        }
      } else {
        Serial.println("[SISTEMA] A música chegou ao fim naturalmente.");
        delete audio;
        audio = nullptr;
        estado_atual = ESPERANDO; 
      }
      break;
    }

    case CHECANDO_PRESENCA: {
      escreverStatusOLED("Ainda está aí?");
      delay(1000); 
      distancia_atual = lerDistanciaSensor();
      if (distancia_atual > 0 && distancia_atual < DISTANCIA_GATILHO) {
        estado_atual = ESCUTANDO_MIC; 
      } else {
        estado_atual = DESPEDIDA;
      }
      break;
    }

    case DESPEDIDA: {
      roboEyes.setMood(DEFAULT);
      falarTextoRafaela("Beleza chefe, estou indo nessa.");
      estado_atual = ESPERANDO; 
      break;
    }
  }
}