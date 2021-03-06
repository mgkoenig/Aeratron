/**********************************************************
 * 
 * AERATRON WEB INTERFACE
 * ----------------------
 * 
 * A simple web interface that provides a remote control functionality 
 * for an AERATRON fan (model AE2+). This application enables the same 
 * features as the original RF remote control (model AE+) 
 * of the manufacturer. 
 * 
 * Used hardware: ESP32 development board
 *                433MHz transmitter module
 * 
 * Author: Matthias Koenig, 2020
 * 
 *********************************************************/

/**********************************************************
 * INCLUDES
 *********************************************************/
#include "WiFi.h"
#include "AsyncTCP.h"
#include "ESPAsyncWebServer.h"
#include "ArduinoJson.h"
#include "SPIFFS.h"
#include "time.h"


/**********************************************************
 * DEFINES
 *********************************************************/
#define VERSION_MAJOR         2
#define VERSION_MINOR         11
#define VERSION_PATCH         9

#define FAN_ADDRESS           0xF0    // static fan address
#define DATA_PIN              27      // pin to RF transmitter module
#define SYSLOG_ENTRY_MAX      10      // max number of syslog entries
#define CHANGELOG_ENTRY_MAX   15      // max number of changelog entries
#define FAULT_TOLERANCE       4       // going to restart after 4 detected errors in a row
#define SYNC_TIME             7200    // given in seconds. time to re-sync the system time
#define IDLE_TIME             6       // given in seconds. delay-time within the main loop
#define STR_BUFFER_LEN        60      // general string buffer length
#define GMT_OFFSET            0       // given in seconds (3600 for GMT+1)
#define DAYLIGHT_OFFSET       0       // given in seconds (3600 for +1h summer time)
#define RAM_MEMORY_MAX        520000  // 520kb sram are available on this device 


/**********************************************************
 * GLOBALS
 *********************************************************/
const char* ssid      = "YOUR_SSID";
const char* password  = "YOUR_PASSWORD";
const char* ntpServer = "pool.ntp.org";

AsyncWebServer server(80);
StaticJsonDocument<(200*SYSLOG_ENTRY_MAX)> syslog;        // allocate memory for the syslog entries
File syslogJsonFile;

char fan_ctrl;
char light_ctrl;
uint8_t err_cnt;
uint8_t prev_err;
uint16_t sync_cnt;


/**********************************************************
 * ENUMS
 *********************************************************/
enum light_state {
  LIGHT_STATE_ON  = 0xDF,
  LIGHT_STATE_OFF = 0x00 
};

enum fan_speed {
  FAN_SPEED_OFF = 0x00,
  FAN_SPEED_1   = 0x01,
  FAN_SPEED_2   = 0x02,
  FAN_SPEED_3   = 0x03,
  FAN_SPEED_4   = 0x04,
  FAN_SPEED_5   = 0x05,
  FAN_SPEED_6   = 0x06,
  FAN_SPEED_ON  = 0x07
};

enum fan_direction {
  FAN_DIRECTION_LEFT = 0x00,
  FAN_DIRECTION_RIGHT = 0x20
};

enum time_format {
  TIMEFORMAT_DAY,
  TIMEFORMAT_DAYDATE,
  TIMEFORMAT_DATE, 
  TIMEFORMAT_DATE_ABBR,
  TIMEFORMAT_DATE_SHORT,
  TIMEFORMAT_TIME, 
  TIMEFORMAT_DATETIME_FULL,
  TIMEFORMAT_DATETIME_SHORT,
  TIMEFORMAT_ABOUT_PAGE
};

enum error_message {
  ERR_NETWORK_NONE,
  ERR_NETWORK_BUSY,
  ERR_NETWORK_CONNECT_FAILED,
  ERR_NETWORK_CONNECTION_LOST,
  ERR_NETWORK_UNAIVAILABLE, 
  ERR_NETWORK_UNKNOWN
};


/**********************************************************
 * FUNCTION PROTOTYPES
 *********************************************************/
void set_light(enum light_state ls);
void set_speed(enum fan_speed fs);
void set_direction(enum fan_direction fd);
void send_command();
String get_date(enum time_format tf);
void write_syslog(const char *evt);
void clear_syslog();
void write_logfile();
void start_wifi();
void error_handling(enum error_message err);


/**********************************************************
 * WEB INTERFACES
 *********************************************************/
const char index_html[] PROGMEM = R"rawliteral(<!DOCTYPE html> 
<html> 

<head> 
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Aeratron Fan Control</title> 
  <link rel="stylesheet" type="text/css" href="styles.css">
</head> 

<body>
<center>
<h2>Aeratron Fan Control</h2>
<p><small><a href="/about">System Information</a></small></p><br>
%CONTROL_PANEL%
</center>
</body>
</html>
)rawliteral";

const char about_html[] PROGMEM = R"rawliteral(<!DOCTYPE html> 
<html> 

<head> 
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Aeratron System Info</title> 
  <link rel="stylesheet" type="text/css" href="styles.css">
</head> 

<body>
<center>
<h2>System Information</h2><br>
%ABOUT_TABLE%
</center>
</body>
</html>
)rawliteral";

const char syslog_html[] PROGMEM = R"rawliteral(<!DOCTYPE html> 
<html> 

<head> 
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Aeratron Syslog</title> 
  <link rel="stylesheet" type="text/css" href="styles.css">
</head> 

<body>
<center>
<h2>System Log</h2><br>
%SYSLOG_TABLE%
</center>
</body>
</html>
)rawliteral";

const char confirm_syslog_deletion_html[] PROGMEM = R"rawliteral(<!DOCTYPE html> 
<html> 

<head> 
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Aeratron Syslog</title> 
  <link rel="stylesheet" type="text/css" href="styles.css">
</head> 

<body>
<center>
<h2>WARNING</h2><br>
<p>This will delete all syslog entries irrevocable.</p>
<p>Are you sure you want to continue?</p><br>
%CONFIRM_SYSLOG_DELETION%
</center>
</body>
</html>
)rawliteral";

const char syslog_deleted_html[] PROGMEM = R"rawliteral(<!DOCTYPE html> 
<html> 
            
<head> 
  %META_REDIRECTION%
  <title>Aeratron Syslog</title> 
  <link rel="stylesheet" type="text/css" href="styles.css">
</head> 

<body>
<center>
<p>Deleting all syslog entries....</p>
<p>You'll be forwarded shortly. If not, please %LINK_HOME%.</p><br>
<div class="loader"></div>
</center>
</body>
</html>
)rawliteral";

const char confirm_reset_html[] PROGMEM = R"rawliteral(<!DOCTYPE html> 
<html> 

<head> 
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Aeratron Reset Confirm</title> 
  <link rel="stylesheet" type="text/css" href="styles.css">
</head> 

<body>
<center>
<h2>WARNING</h2><br>
<p>This will restart the device.</p>
<p>Are you sure you want to continue?</p><br>
%CONFIRM_RESET%
</center>
</body>
</html>
)rawliteral";

const char reset_progress_html[] PROGMEM = R"rawliteral(<!DOCTYPE html> 
<html> 

<head> 
  %META_REDIRECTION%
  <title>Aeratron Reset Confirm</title> 
  <link rel="stylesheet" type="text/css" href="styles.css">
</head> 

<body>
<center>
<p>Device is resetting. You will be forwarded shortly...</p><br>
<div class="loader"></div>
</center>
</body>
</html>
)rawliteral";


/**********************************************************
 * PAGE BUILDER
 *********************************************************/
String page_builder(const String& var) 
{ 
  if(var == "CONTROL_PANEL") {
    String panel = "";

    if ((fan_ctrl & 0x0F) == FAN_SPEED_OFF) {
        panel += "<button class=\"switcher\" onclick=\"window.location.href='/fan-on';\">Fan ON</button>";
        panel += '\n';
        panel += "<button class=\"switcher\" style=\"background-color:#4CAF50\" onclick=\"window.location.href='/fan-0';\">Fan OFF</button><br>";
    } else {
        panel += "<button class=\"switcher\" style=\"background-color:#4CAF50\" onclick=\"window.location.href='/fan-on';\">Fan ON</button>";
        panel += '\n';
        panel += "<button class=\"switcher\" onclick=\"window.location.href='/fan-0';\">Fan OFF</button><br>";
    }
    
    panel += '\n';
    if (light_ctrl == LIGHT_STATE_ON) {
        panel += "<button class=\"switcher\" style=\"background-color:#4CAF50\" onclick=\"window.location.href='/light-on';\">Light ON</button>";
        panel += '\n';
        panel += "<button class=\"switcher\" onclick=\"window.location.href='/light-off';\">Light OFF</button><br>";
    } else {
        panel += "<button class=\"switcher\" onclick=\"window.location.href='/light-on';\">Light ON</button>";
        panel += '\n';
        panel += "<button class=\"switcher\" style=\"background-color:#4CAF50\" onclick=\"window.location.href='/light-off';\">Light OFF</button><br>"; 
    }
    
    panel += '\n';
    if ((fan_ctrl & 0xF0) == FAN_DIRECTION_LEFT) {
        panel += "<button class=\"switcher\" title=\"Summer Season\" style=\"background-color:#4CAF50\" onclick=\"window.location.href='/fan-left';\">Rotate Left</button>";
        panel += '\n';
        panel += "<button class=\"switcher\" title=\"Winter Season\" onclick=\"window.location.href='/fan-right';\">Rotate Right</button><br>";
    } else {
        panel += "<button class=\"switcher\" title=\"Summer Season\" onclick=\"window.location.href='/fan-left';\">Rotate Left</button>";
        panel += '\n';
        panel += "<button class=\"switcher\" title=\"Winter Season\" style=\"background-color:#4CAF50\" onclick=\"window.location.href='/fan-right';\">Rotate Right</button><br>";
    }
    
    panel += '\n';
    if ((fan_ctrl & 0x0F) == FAN_SPEED_1) {
        panel += "<button class=\"level\" title=\"55rpm (4.4W)\" style=\"background-color:#4CAF50\" onclick=\"window.location.href='/fan-1';\">1</button>";
    } else {
        panel += "<button class=\"level\" title=\"55rpm (4.4W)\" onclick=\"window.location.href='/fan-1';\">1</button>";     
    } 
    
    panel += '\n';    
    if ((fan_ctrl & 0x0F) == FAN_SPEED_2) {
        panel += "<button class=\"level\" title=\"85rpm (5.6W)\" style=\"background-color:#4CAF50\" onclick=\"window.location.href='/fan-2';\">2</button>";
    } else {
        panel += "<button class=\"level\" title=\"85rpm (5.6W)\" onclick=\"window.location.href='/fan-2';\">2</button>";          
    }     
    
    panel += '\n';    
    if ((fan_ctrl & 0x0F) == FAN_SPEED_3) {
        panel += "<button class=\"level\" title=\"110rpm (7.6W)\" style=\"background-color:#4CAF50\" onclick=\"window.location.href='/fan-3';\">3</button>";
    } else {
        panel += "<button class=\"level\" title=\"110rpm (7.6W)\" onclick=\"window.location.href='/fan-3';\">3</button>";          
    } 
    
    panel += '\n';    
    if ((fan_ctrl & 0x0F) == FAN_SPEED_4) {
       panel += "<button class=\"level\" title=\"130rpm (10.1W)\" style=\"background-color:#4CAF50\" onclick=\"window.location.href='/fan-4';\">4</button>";
    } else {
       panel += "<button class=\"level\" title=\"130rpm (10.1W)\" onclick=\"window.location.href='/fan-4';\">4</button>";          
    } 
    
    panel += '\n';    
    if ((fan_ctrl & 0x0F) == FAN_SPEED_5) {
       panel += "<button class=\"level\" title=\"155rpm (13W)\" style=\"background-color:#4CAF50\" onclick=\"window.location.href='/fan-5';\">5</button>";
    } else {
       panel += "<button class=\"level\" title=\"155rpm (13W)\" onclick=\"window.location.href='/fan-5';\">5</button>";          
    } 
    
    panel += '\n';    
    if ((fan_ctrl & 0x0F) == FAN_SPEED_6) {
       panel += "<button class=\"level\" title=\"185rpm (17.3W)\" style=\"background-color:#4CAF50\" onclick=\"window.location.href='/fan-6';\">6</button>";
    } else {
       panel += "<button class=\"level\" title=\"185rpm (17.3W)\" onclick=\"window.location.href='/fan-6';\">6</button>";          
    }   
    
    return panel;
  }

  if(var == "ABOUT_TABLE") {
    uint32_t used_ram;
    char fan_addr[5];
    String a_table = "";
    
    a_table += "<table class=about>\n";
    a_table += "<tr>\n";
    a_table += "  <td>Firmware</td>\n";
    a_table += "  <td>";
    a_table += VERSION_MAJOR;
    a_table += ".";
    a_table += VERSION_MINOR;
    a_table += ".";
    a_table += VERSION_PATCH;
    a_table += "</td>\n</tr>\n";

    sprintf(fan_addr, "0x%.2X", FAN_ADDRESS);
    a_table += "<tr>\n";
    a_table += "  <td>Fan Address</td>\n";
    a_table += "  <td>";
    a_table += fan_addr; 
    a_table += "</td>\n</tr>\n";    

    a_table += "<tr>\n";
    a_table += "  <td>IP Address</td>\n";
    a_table += "  <td>" + WiFi.localIP().toString(); 
    a_table += "</td>\n</tr>\n";  

    a_table += "<tr>\n";
    a_table += "  <td>System Time</td>\n";
    a_table += "  <td>" + get_date(TIMEFORMAT_ABOUT_PAGE); 
    a_table += "</td>\n</tr>\n";    
    
    a_table += "<tr>\n";
    a_table += "  <td>Version History</td>\n";
    a_table += "  <td><a href=\"/changelog\">Changelog</a>";
    a_table += "</td>\n</tr>\n";    
    
    a_table += "<tr>\n";
    a_table += "  <td>Fault Report</td>\n";
    a_table += "  <td><a href=\"/syslog\">Syslog</a>";
    a_table += "</td>\n</tr>\n";    
    
    used_ram = (100 - ((100 * (ESP.getFreeHeap()) / RAM_MEMORY_MAX)));  
    a_table += "<tr>\n";
    a_table += "  <td>RAM Usage</td>\n";
    a_table += "  <td>";
    a_table += used_ram;
    a_table += "%"; 
    a_table += "</td>\n</tr>\n";    

    a_table += "<tr>\n";
    a_table += "  <td>Device Restart</td>\n";
    a_table += "  <td><a href=\"/confirm_reset\">Restart</a>";
    a_table += "</td>\n</tr>\n";   
    
    a_table += "</table>";

    return a_table;
  }

  if(var == "SYSLOG_TABLE") {
    uint8_t i, idx, cnt;
    String sl_table = "";

    idx = syslog["index"];
    cnt = syslog["evtcnt"];

    sl_table += "<table class=log>\n";
    sl_table += "<colgroup>\n";
    sl_table += "<col style=\"width:10%%\"/>\n";
    sl_table += "<col style=\"width:35%%\"/>\n";
    sl_table += "<col style=\"width:55%%\"/>\n";
    sl_table += "</colgroup>\n";
    sl_table += "<tr>\n";
    sl_table += "<th>ID</th>\n";
    sl_table += "<th>Date</th>\n";
    sl_table += "<th>Event</th>\n";
    sl_table += "</tr>\n"; 

    if (cnt < SYSLOG_ENTRY_MAX) {          // normal enumeration of the table
      for (i=0; i<SYSLOG_ENTRY_MAX; i++) {
        sl_table += "<tr>\n";
        sl_table += "<td>";
        sl_table += syslog["entry"][i]["id"].as<int>();
        sl_table += "</td>\n";
        sl_table += "<td>";
        sl_table += syslog["entry"][i]["date"].as<char*>();
        sl_table += "</td>\n";
        sl_table += "<td>";
        sl_table += syslog["entry"][i]["event"].as<char*>();
        sl_table += "</td>\n";
        sl_table += "</tr>\n";     
      } 
    } 
    else {                              // take ring buffer enumeration into account
      for (i=0; i<SYSLOG_ENTRY_MAX; i++) { 
        sl_table += "<tr>\n";
        sl_table += "<td>";
        sl_table += syslog["entry"][idx]["id"].as<int>();
        sl_table += "</td>\n";
        sl_table += "<td>";
        sl_table += syslog["entry"][idx]["date"].as<char*>();
        sl_table += "</td>\n";
        sl_table += "<td>";
        sl_table += syslog["entry"][idx]["event"].as<char*>();
        sl_table += "</td>\n";
        sl_table += "</tr>\n";   
        
        idx++;
        if (idx >= SYSLOG_ENTRY_MAX)  
          idx = 0;
      } 
    }

    sl_table += "<tr>\n";
    sl_table += "<td colspan=\"3\"><a href=\"/confirm_deletion\">Delete all entries</a></td>\n";
    sl_table += "</tr>\n";  
    sl_table += "</table>";

    return sl_table;
  }

  if(var == "CONFIRM_SYSLOG_DELETION") {
    String csd_panel = "";

    csd_panel += "<button class=\"switcher\" onclick=\"window.location.href='http://"; 
    csd_panel += WiFi.localIP().toString();
    csd_panel += "';\">Cancel</button>";
    csd_panel += '\n';
    csd_panel += "<button class=\"switcher\" onclick=\"window.location.href='/syslog_clear';\">Delete</button><br>";

    return csd_panel;
  }

  if(var == "CONFIRM_RESET") {
    String res_panel = "";

    res_panel += "<button class=\"switcher\" onclick=\"window.location.href='http://"; 
    res_panel += WiFi.localIP().toString();
    res_panel += "';\">Cancel</button>";
    res_panel += '\n';
    res_panel += "<button class=\"switcher\" onclick=\"window.location.href='/reset';\">Reset</button><br>";

    return res_panel;
  }

  if(var == "META_REDIRECTION") {
    String meta = "";

    // redirect to start page after 5 seconds
    meta += "<meta http-equiv=\"refresh\" content=\"3;url=http://"; 
    meta += WiFi.localIP().toString();
    meta += "\"/>";

    return meta;
  }

  if(var == "LINK_HOME") {
    String link = "";

    link += "<a href=\"http://";
    link += WiFi.localIP().toString();
    link += "\">click here</a>";

    return link;
  }

  return String();
}


/**********************************************************
 * SETUP
 *********************************************************/
void setup() 
{
  DeserializationError deser_err;
  char header[STR_BUFFER_LEN];
  int i;
 
  Serial.begin(115200);

  snprintf(header, STR_BUFFER_LEN, "Aeratron Remote Web Client (Firmware: %d.%d.%d)", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
  Serial.println(header);
  for (i=0; i<strlen(header); i++) {
    Serial.print("=");
  }  
  Serial.print("\n\n");

  // initialize internal FS
  Serial.print("Mounting file system.. ");
  if(!SPIFFS.begin()) {
    Serial.println("An Error has occurred while mounting SPIFFS.");
    return;
  }
  Serial.println(" Done.");

  Serial.print("Initialize state variables.. ");
  fan_ctrl = 0x00;
  light_ctrl = 0x00;
  sync_cnt = 0;
  err_cnt = 0;
  prev_err = ERR_NETWORK_NONE;
  Serial.println(" Done.");

  // Init pin
  Serial.print("Initialize output pins.. ");
  pinMode(DATA_PIN, OUTPUT);
  Serial.println(" Done.");

  Serial.print("Looking for favicon.. ");
  if (SPIFFS.exists("/favicon.png")) {
      Serial.println(" Found.");
  }
  else 
    Serial.println(" Error: Favicon not found!");

  Serial.print("Looking for touchicon.. ");
  if (SPIFFS.exists("/touchicon.png")) {
      Serial.println(" Found.");
  }
  else 
    Serial.println(" Error: Touchicon not found!");

  Serial.print("Checking for styles.css.. ");
  if (SPIFFS.exists("/styles.css")) {
      Serial.println(" Found.");
  }
  else 
    Serial.println(" Error: no stylesheet found!");

  Serial.print("Checking for syslog.json.. ");
  if (SPIFFS.exists("/syslog.json")) {
      Serial.println(" Found.");
  }
  else 
    Serial.println(" Error: syslog.json not found!");

  Serial.print("Loading syslog.json.. ");
  syslogJsonFile = SPIFFS.open("/syslog.json", "r");
  deser_err = deserializeJson(syslog, syslogJsonFile);
  if (deser_err)
    Serial.println(F("Failed to read syslog.json. No syslog available!"));
  syslogJsonFile.close();  
  Serial.println(" Done.");

  // just init wifi. state monitoring and error handling are done within the main loop
  Serial.print("Connecting to WiFi.. ");
  start_wifi();
  Serial.print(" Done. [");
  Serial.print(WiFi.localIP());
  Serial.println("]");
 
  Serial.print("Retrieving time and date.. ");
  configTime(GMT_OFFSET, DAYLIGHT_OFFSET, ntpServer);
  Serial.print(" Done. [");
  Serial.print(get_date(TIMEFORMAT_DATE_ABBR));
  Serial.println("]");

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    //request->send(200, "text/plain", "Main Page.");
    request->send_P(200, "text/html", index_html, page_builder);
  });

  server.on("/styles.css", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/styles.css", "text/css");
  });
 
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/favicon.png", "image/png");
  });

  server.on("/apple-touch-icon.png", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/touchicon.png", "image/png");
  });

  server.on("/about", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", about_html, page_builder);
  });

  server.on("/changelog", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/changelog.html", "text/html");
  });

  server.on("/syslog", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", syslog_html, page_builder);
  });
  
  server.on("/confirm_deletion", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", confirm_syslog_deletion_html, page_builder);
  });

  server.on("/syslog_clear", HTTP_GET, [](AsyncWebServerRequest *request){
    clear_syslog();
    request->send_P(200, "text/html", syslog_deleted_html, page_builder);
  });

  server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request){
    request->redirect("/reset_progress");
    write_syslog("System reset requested by user.");
    ESP.restart();
  });

  server.on("/confirm_reset", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", confirm_reset_html, page_builder);
  });

  server.on("/reset_progress", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", reset_progress_html, page_builder);
  });
 
  server.on("/fan-on", HTTP_GET, [](AsyncWebServerRequest *request){
    set_speed(FAN_SPEED_ON);
    request->send_P(200, "text/html", index_html, page_builder);
  });

  server.on("/fan-0", HTTP_GET, [](AsyncWebServerRequest *request){
    set_speed(FAN_SPEED_OFF);
    request->send_P(200, "text/html", index_html, page_builder);
  });

  server.on("/fan-1", HTTP_GET, [](AsyncWebServerRequest *request){
    set_speed(FAN_SPEED_1);
    request->send_P(200, "text/html", index_html, page_builder);
  });

  server.on("/fan-2", HTTP_GET, [](AsyncWebServerRequest *request){
    set_speed(FAN_SPEED_2);
    request->send_P(200, "text/html", index_html, page_builder);
  });

  server.on("/fan-3", HTTP_GET, [](AsyncWebServerRequest *request){
    set_speed(FAN_SPEED_3);
    request->send_P(200, "text/html", index_html, page_builder);
  });

  server.on("/fan-4", HTTP_GET, [](AsyncWebServerRequest *request){
    set_speed(FAN_SPEED_4);
    request->send_P(200, "text/html", index_html, page_builder);
  });

  server.on("/fan-5", HTTP_GET, [](AsyncWebServerRequest *request){
    set_speed(FAN_SPEED_5);
    request->send_P(200, "text/html", index_html, page_builder);
  });

  server.on("/fan-6", HTTP_GET, [](AsyncWebServerRequest *request){
    set_speed(FAN_SPEED_6);
    request->send_P(200, "text/html", index_html, page_builder);
  });

  server.on("/fan-left", HTTP_GET, [](AsyncWebServerRequest *request){
    set_direction(FAN_DIRECTION_LEFT);
    request->send_P(200, "text/html", index_html, page_builder);
  });

  server.on("/fan-right", HTTP_GET, [](AsyncWebServerRequest *request){
    set_direction(FAN_DIRECTION_RIGHT);
    request->send_P(200, "text/html", index_html, page_builder);
  });

  server.on("/light-on", HTTP_GET, [](AsyncWebServerRequest *request){
    set_light(LIGHT_STATE_ON);
    request->send_P(200, "text/html", index_html, page_builder);
  });

  server.on("/light-off", HTTP_GET, [](AsyncWebServerRequest *request){
    set_light(LIGHT_STATE_OFF);
    request->send_P(200, "text/html", index_html, page_builder);
  });
  
  server.begin();

  write_syslog("Device up and running.");  
}


/**********************************************************
 * LOOP
 *********************************************************/
void loop() 
{
  wl_status_t wifi_state;

  // checking for the WiFi connection regularly 
  wifi_state = WiFi.status();  

  switch (wifi_state)
  {
    case WL_CONNECTED: error_handling(ERR_NETWORK_NONE); break;
    case WL_NO_SHIELD:
    case WL_IDLE_STATUS:
    case WL_NO_SSID_AVAIL:
    case WL_SCAN_COMPLETED: error_handling(ERR_NETWORK_BUSY); break;
    case WL_CONNECT_FAILED: error_handling(ERR_NETWORK_CONNECT_FAILED); break;
    case WL_CONNECTION_LOST: error_handling(ERR_NETWORK_CONNECTION_LOST); break;
    case WL_DISCONNECTED: error_handling(ERR_NETWORK_UNAIVAILABLE); break;
    default: error_handling(ERR_NETWORK_UNKNOWN); break; 
  }

  // sync the clock from time to time to avoid a drift 
  sync_cnt++;
  if (sync_cnt >= (SYNC_TIME / IDLE_TIME)) {
    configTime(GMT_OFFSET, DAYLIGHT_OFFSET, ntpServer);
    sync_cnt = 0;
  }

  delay(1000 * IDLE_TIME);  
}


/**********************************************************
 * FUNCTIONS
 *********************************************************/
void set_light(enum light_state ls)
{
  light_ctrl = ls;
  send_command();
}

void set_direction (enum fan_direction fd)
{
  fan_ctrl &= 0x0F;
  fan_ctrl |= fd;  
  send_command();
}

void set_speed (enum fan_speed fs)
{
  static enum fan_speed prev_speed = FAN_SPEED_1;

  switch (fs)
  {
    case FAN_SPEED_1:
    case FAN_SPEED_2:
    case FAN_SPEED_3:
    case FAN_SPEED_4:
    case FAN_SPEED_5:
    case FAN_SPEED_6:   prev_speed = fs; break;
    case FAN_SPEED_ON:  fs = prev_speed; break;
    case FAN_SPEED_OFF: break;  
    default: break;
  }

  fan_ctrl &= 0xF0;
  fan_ctrl |= fs;  
  
  send_command();
}

void send_command ()
{
  int i, j, k, l;
  char fan_command[3];

  fan_command[0] = FAN_ADDRESS;
  fan_command[1] = fan_ctrl;
  fan_command[2] = light_ctrl;

  // repeat each message 3x7 times
  for (l=0; l<3; l++) {
    for (k=0; k<7; k++) {
        // Start bit
        digitalWrite(DATA_PIN, LOW);          // sets the pin on
        delayMicroseconds(500);     
        digitalWrite(DATA_PIN, HIGH);         // sets the pin off
        delayMicroseconds(1000);    
        
        for (j=0; j<3; j++) {                 // for each command byte
          for (i=7; i>=0; i--) {              // each bit within the current byte (MSB first)
            if (fan_command[j] & (1 << i)) {  // sending a logic 1
              digitalWrite(DATA_PIN, LOW);      
              delayMicroseconds(500);     
              digitalWrite(DATA_PIN, HIGH);     
              delayMicroseconds(1000);    
            }
            else {                            // or sending a logic 0
              digitalWrite(DATA_PIN, LOW);      
              delayMicroseconds(1000);     
              digitalWrite(DATA_PIN, HIGH);     
              delayMicroseconds(500);   
            }
          }
        }
    
        digitalWrite(DATA_PIN, LOW); 
        delay(6);  
      }
      delay(12);  
    }
}

String get_date(enum time_format tf)
{
  char timeStringBuff[STR_BUFFER_LEN];
  struct tm timeinfo;
  
  if(!getLocalTime(&timeinfo)){
    strcpy(timeStringBuff, "No time available");
  }
  else
  {
    switch (tf)
    {
      case TIMEFORMAT_DAY: strftime(timeStringBuff, sizeof(timeStringBuff), "%A", &timeinfo); break;
      case TIMEFORMAT_DAYDATE: strftime(timeStringBuff, sizeof(timeStringBuff), "%A, %d. %B %Y", &timeinfo); break;
      case TIMEFORMAT_DATE: strftime(timeStringBuff, sizeof(timeStringBuff), "%d. %B %Y", &timeinfo); break;
      case TIMEFORMAT_DATE_ABBR: strftime(timeStringBuff, sizeof(timeStringBuff), "%d. %b. %Y", &timeinfo); break;
      case TIMEFORMAT_DATE_SHORT: strftime(timeStringBuff, sizeof(timeStringBuff), "%d.%m.%Y", &timeinfo); break;
      case TIMEFORMAT_TIME: strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M:%S UTC", &timeinfo); break;
      case TIMEFORMAT_DATETIME_FULL: strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d %H:%M:%S UTC", &timeinfo); break;
      case TIMEFORMAT_DATETIME_SHORT: strftime(timeStringBuff, sizeof(timeStringBuff), "%d.%m.%Y - %H:%M UTC", &timeinfo); break;
      case TIMEFORMAT_ABOUT_PAGE: strftime(timeStringBuff, sizeof(timeStringBuff), "%d. %b. %Y<br>%H:%M UTC", &timeinfo); break;
      default: strcpy(timeStringBuff, "Invalid time format");
    }       
  }
  String time_string(timeStringBuff);
  return time_string;
}

void write_syslog(const char *evt)
{
  uint8_t idx;
  uint32_t cnt;

  idx = syslog["index"];
  cnt = syslog["evtcnt"];
  cnt++;
  
  if (cnt >= (UINT32_MAX-1))
  {
    cnt = 1;

    syslog["entry"][idx]["id"] = cnt;
    syslog["entry"][idx]["date"] = get_date(TIMEFORMAT_DATETIME_FULL);
    syslog["entry"][idx]["event"] = "Event counter overflow. Reset evtcnt to 1.";

    cnt++;
    idx++;
    if(idx >= SYSLOG_ENTRY_MAX) {
      idx = 0;
    }
  }

  syslog["entry"][idx]["id"] = cnt;
  syslog["entry"][idx]["date"] = get_date(TIMEFORMAT_DATETIME_FULL);
  syslog["entry"][idx]["event"] = evt;

  idx++;
  if(idx >= SYSLOG_ENTRY_MAX) {
    idx = 0;
  }
  
  syslog["index"] = idx;
  syslog["evtcnt"] = cnt;  
 
  write_logfile(); 
}

void clear_syslog()
{
    uint8_t i;

    syslog["index"] = 0;
    syslog["evtcnt"] = 0; 

    for (i=0; i<SYSLOG_ENTRY_MAX; i++)
    {
      syslog["entry"][i]["id"] = 0;
      syslog["entry"][i]["date"] = "1970-01-01 00:00:00 UTC";
      syslog["entry"][i]["event"] = "no event";
    }

    write_syslog("All syslog entries deleted.");
    Serial.println("All syslog entries deleted!");  
}

void write_logfile()
{
  syslogJsonFile = SPIFFS.open("/syslog.json", "w");
  if (serializeJson(syslog, syslogJsonFile) == 0) {
    Serial.println(F("Failed to write logfile!"));
  }
  syslogJsonFile.close();  
}

void start_wifi()
{
  uint8_t attempts = 0;

  WiFi.disconnect();
  //WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid, password);

  while ((WiFi.status() != WL_CONNECTED) && (attempts < 30)) {
    delay(500);
    attempts++;
  }
}

void error_handling(enum error_message err)
{
  switch (err)
  {
    case ERR_NETWORK_NONE:  if (err_cnt > 0) {
                              err_cnt = 0; 
                            } break;
    case ERR_NETWORK_BUSY: err_cnt++; break;
    case ERR_NETWORK_CONNECT_FAILED: err_cnt++; start_wifi(); break;
    case ERR_NETWORK_CONNECTION_LOST: err_cnt++; start_wifi(); break;
    case ERR_NETWORK_UNAIVAILABLE: err_cnt++; start_wifi(); break;
    case ERR_NETWORK_UNKNOWN: err_cnt++; break;
    default: err_cnt++; break;
  }

  if (err != prev_err) {
    switch (err)
    {
      case ERR_NETWORK_NONE: write_syslog("Re-established network connection."); break;
      case ERR_NETWORK_BUSY: write_syslog("Network module busy."); break;
      case ERR_NETWORK_CONNECT_FAILED: write_syslog("Failed to connect to network."); break;
      case ERR_NETWORK_CONNECTION_LOST: write_syslog("Lost network connection."); break;
      case ERR_NETWORK_UNAIVAILABLE: write_syslog("Disconnected from network."); break;            
      case ERR_NETWORK_UNKNOWN: write_syslog("Undefined network state discovered."); break;                                       
      default: write_syslog("Unknown error occurred."); break;                                        
    }
    prev_err = err;
  }

  if (err_cnt > FAULT_TOLERANCE) {
    write_syslog("Permanent fault detected. Restarting ESP.");
    ESP.restart();
  }
}
