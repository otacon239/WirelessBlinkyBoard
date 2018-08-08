#include "LedControl.h"
#include <pgmspace.h>
#include <WiFi.h>
#include <NTPClient.h>
#include "FS.h"
#include "SPIFFS.h"
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

LedControl lc = LedControl(23,22,14,1);  // Pins: DIN,CLK,CS, # of Display connected
#define WIDTH 8
#define HEIGHT 8
int intensity = 0;
#define VBATPIN A13
float measuredvbat;

const char* ssid = "[SSID]";
const char* password = "[Password]";

AsyncWebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "time.nist.gov", -25200, 60000);
int func = 0; // Set default startup mode
int funcDelay = 0;

// Client Variables
char linebuf[80];
int charcount=0;

// For sinWave()
float phaseA = 0;
float phaseB = 0;

// For postMode()
byte paramModeData[8]; // Need to update this to be [WIDTH][HEIGHT] for scalable display

String filePath = "/inputs.html";
String fileData;

float getVoltage() {
	float x = analogRead(VBATPIN);
	x *= 2; // Adafruit divides by 2 by default
	x /= 1.1; // Reference voltage is 1.1V
	x /= 1024; // convert to voltage
	return x;
}

void binaryClock() { // This function was specifically designed for an 8x8 display
	timeClient.update();
	for (int i = 0; i < 4; i++)
		lc.setRow(0, i, int(WiFi.localIP()[i]));
	lc.setRow(0, 4, 0);
	lc.setLed(0, 4, timeClient.getDay(), true);
	lc.setRow(0, 5, timeClient.getHours());
	lc.setRow(0, 6, timeClient.getMinutes());
	lc.setRow(0, 7, timeClient.getSeconds());

	if (getVoltage() > 4.15) // Check if battery is charged (above 4.15V)
		lc.setLed(0, 4, 7, true);
}

void sinWave() {
	lc.clearDisplay(0);
	for (int i = 0; i < 8; i++) {
		lc.setLed(0, round((sin(i*.5 + phaseA)*3.5)+3.5), i, true);
		lc.setLed(0, round((sin(i*.5 + phaseB)*3.5)+3.5), i, true);
	}
	phaseA = phaseA + 1/(2*PI)*.05;
	phaseB = phaseB + 1/(2*PI)*1.1*.05;
}

void paramMode() {
	for (int i = 0; i < sizeof(paramModeData); i++)
		lc.setColumn(0, i, paramModeData[i]);
}

void errorMode() { // Display letter E when an invalid function is set
	char letterE[] = { 0x41, 0x7F, 0x7F, 0x49, 0x5D, 0x41, 0x63, 0x00 }; // 'E'
	for (int i = 0; i < sizeof(letterE); i++)
		lc.setColumn(0, i, letterE[i]);
}

void setup() {
	SPIFFS.begin();
	File file = SPIFFS.open(filePath);

	Serial.begin(115200);
	Serial.println("\n\nInitializing Display...");
	lc.shutdown(0,false);  // Wake up displays
	lc.setIntensity(0, intensity);  // Set intensity levels
	lc.clearDisplay(0);  // Clear Displays

	// Connect to Wifi
	Serial.print("\n\nConnecting to \"");
	Serial.print(ssid);
	Serial.print('\"');

	WiFi.begin(ssid, password);
	while(WiFi.status() != WL_CONNECTED) {
		// For pretty sin wave loading screen OwO
		lc.clearDisplay(0);
		for (int i = 0; i < 8; i++)
			lc.setLed(0, round((sin(i*.5 + phaseA)*3.5)+3.5), i, true);
		phaseA = phaseA + 1/(2*PI);
		delay(20);

		timeClient.begin();
	}

	lc.clearDisplay(0);

	Serial.println("");
	Serial.println("WiFi connected");
	Serial.print("IP address: ");
	Serial.println(WiFi.localIP());
	if (SPIFFS.exists(filePath)) {
		Serial.println("File exists!");
		while(file.available()){
				fileData += char(file.read());
				//sprintf(fileData, "%s%s", fileData, file.read());
		};
		//Serial.println(fileData);
	}
	Serial.print("VBat: " ); Serial.println(getVoltage());

	server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
		request->send(200, "text/html", fileData +
		// Adding brightness number field is next step
		"<br/>Brightness:" +
			"<form id=\"brightnessSlider\" action=\"/intensity\">" +
			"<input type=\"range\" name=\"value\"\n" +
			"min=\"0\" max=\"15\" step=\"1\" value=\"" + intensity + "\">\n" +
			"<input type=\"submit\" value=\"GO\">" +
			"</form>" +
		"<br/>Current Pattern: " + String(func) +
		"<br/>Current Battery Voltage: " + String(getVoltage()) + " V"
		"\n</body>\n</html>");
	});
	// 10.10.10.132/param?value=1
	server.on("/mode", HTTP_GET, [](AsyncWebServerRequest *request){
		if(request->hasParam("func")) {
			AsyncWebParameter* p = request->getParam("func");
			Serial.println("Mode " + p->value() + " Set");
			func = p->value().toInt();
			request->redirect("/");
		} else {
			request->redirect("/");
			Serial.println("User was a gibbon. They didn't use the func param.");
		}
	});

	server.on("/param", HTTP_GET, [](AsyncWebServerRequest *request){
		if(request->hasParam("value") && request->hasParam("column")) {
			AsyncWebParameter* v = request->getParam("value");
			AsyncWebParameter* c = request->getParam("column");
			Serial.println("Param update. Column: " + c->value() + ", Value: " + v->value());
			paramModeData[std::min(int(c->value().toInt()),HEIGHT-1)] = byte(v->value().toInt()); // Sorry this statement looks horrible
			request->redirect("/");
		} else if (request->hasParam("x") && request->hasParam("y") && request->hasParam("value")) {
			AsyncWebParameter* x = request->getParam("x");
			AsyncWebParameter* y = request->getParam("y");
			AsyncWebParameter* value = request->getParam("value");
			Serial.println("Param update: (" + x->value() + "," + y->value() + "," + value->value() + ")");
			int X = std::min(int(x->value().toInt()),WIDTH-1);
			int Y = std::min(int(y->value().toInt()),HEIGHT-1);
			bool Value = value->value().toInt();
			if (Value) { // See https://stackoverflow.com/a/47990
				// This is where magical bit-shifting happens - I hardly understand this
				paramModeData[X] |= 0x80 >> Y;
			} else {
				paramModeData[X] &= ~(0x80 >> Y);
			}

			request->redirect("/");
		} else {
			request->redirect("/");
			Serial.println("User was a gibbon. They didn't use the func param.");
		}
	});

	server.on("/clearparams", HTTP_GET, [](AsyncWebServerRequest *request){
		for (int i = 0; i < sizeof(paramModeData); i++)
			paramModeData[i]= 0;
		request->redirect("/");
	});

	server.on("/intensity", HTTP_GET, [](AsyncWebServerRequest *request){
		if(request->hasParam("value")) {
			AsyncWebParameter* p = request->getParam("value");
			Serial.println("Intensity set to: " + p->value());
			intensity = p->value().toInt();
			lc.setIntensity(0, intensity);
			request->redirect("/");
		} else {
			request->redirect("/");
			Serial.println("User was a gibbon. They didn't use the func param.");
		}
	});

	server.begin();
}

void loop() {
	switch(func){
		case 0:
			binaryClock();
			break;
		case 1:
			sinWave();
			break;
		case 2:
			paramMode();
			break;
		default:
			errorMode();
			break;
	}
	delay(1);
}
