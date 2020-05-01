#include "EspMQTTClient.h"


// =============== Constructor / destructor ===================

// MQTT only (no wifi connection attempt)
EspMQTTClient::EspMQTTClient(
  const char* mqttServerIp,
  const short mqttServerPort,
  const char* mqttClientName) :
  EspMQTTClient(NULL, NULL, mqttServerIp, NULL, NULL, mqttClientName, mqttServerPort)
{
}

EspMQTTClient::EspMQTTClient(
  const char* mqttServerIp,
  const short mqttServerPort,
  const char* mqttUsername,
  const char* mqttPassword,
  const char* mqttClientName) :
  EspMQTTClient(NULL, NULL, mqttServerIp, mqttUsername, mqttPassword, mqttClientName, mqttServerPort)
{
}

// Wifi and MQTT handling
EspMQTTClient::EspMQTTClient(
  const char* wifiSsid,
  const char* wifiPassword,
  const char* mqttServerIp,
  const char* mqttClientName,
  const short mqttServerPort) :
  EspMQTTClient(wifiSsid, wifiPassword, mqttServerIp, NULL, NULL, mqttClientName, mqttServerPort)
{
}

EspMQTTClient::EspMQTTClient(
  const char* wifiSsid,
  const char* wifiPassword,
  const char* mqttServerIp,
  const char* mqttUsername,
  const char* mqttPassword,
  const char* mqttClientName,
  const short mqttServerPort) :
  _wifiSsid(wifiSsid),
  _wifiPassword(wifiPassword),
  _mqttServerIp(mqttServerIp),
  _mqttUsername(mqttUsername),
  _mqttPassword(mqttPassword),
  _mqttClientName(mqttClientName),
  _mqttServerPort(mqttServerPort),
  _mqttClient(mqttServerIp, mqttServerPort, _wifiClient)
{
  // WiFi connection
  _wifiConnected = false;
  _lastWifiConnectionAttemptMillis = 0;
  _lastWifiConnectionSuccessMillis = 0;

  // MQTT client
  _topicSubscriptionListSize = 0;
  _mqttConnected = false;
  _lastMqttConnectionAttemptMillis = 0;
  _mqttLastWillTopic = 0;
  _mqttLastWillMessage = 0;
  _mqttLastWillRetain = false;
  _mqttCleanSession = true;
  _mqttClient.setCallback([this](char* topic, byte* payload, unsigned int length) {this->mqttMessageReceivedCallback(topic, payload, length);});

  // Web updater
  _updateServerAddress = NULL;
  _httpServer = NULL;
  _httpUpdater = NULL;

  // other
  _enableSerialLogs = false;
  _connectionEstablishedCallback = onConnectionEstablished;
  _delayedExecutionListSize = 0;
  _connectionEstablishedCount = 0;
}

EspMQTTClient::~EspMQTTClient()
{
  if (_httpServer != NULL)
    delete _httpServer;
  if (_httpUpdater != NULL)
    delete _httpUpdater;
}


// =============== Configuration functions, most of them must be called before the first loop() call ==============

void EspMQTTClient::enableDebuggingMessages(const bool enabled)
{
  _enableSerialLogs = enabled;
}

void EspMQTTClient::enableMACaddress_for_ClientName(const bool enabled)
{
  _enableMACaddress = enabled;
}

void EspMQTTClient::enableCustomAuthentication(const bool enabled)
{
  _enableAuthentication = enabled;
}

void EspMQTTClient::enableMQTTConnect(const bool enabled)
{
  _enableMQTTConnect = enabled;
}

void EspMQTTClient::enableHTTPWebUpdater(const char* username, const char* password, const char* address)
{
  if (_httpServer == NULL)
  {
    _httpServer = new WebServer(80);
    _httpUpdater = new ESPHTTPUpdateServer(_enableSerialLogs);
    _updateServerUsername = (char*)username;
    _updateServerPassword = (char*)password;
    _updateServerAddress = (char*)address;
  }
  else if (_enableSerialLogs)
    Serial.print("SYS! You can't call enableHTTPWebUpdater() more than once !\n");
}

void EspMQTTClient::enableHTTPWebUpdater(const char* address)
{
  if(_mqttUsername == NULL || _mqttPassword == NULL)
    enableHTTPWebUpdater("", "", address);
  else
    enableHTTPWebUpdater(_mqttUsername, _mqttPassword, address);
}

void EspMQTTClient::enableMQTTPersistence()
{
  _mqttCleanSession = false;
}

void EspMQTTClient::enableLastWillMessage(const char* topic, const char* message, const bool retain)
{
  _mqttLastWillTopic = (char*)topic;
  _mqttLastWillMessage = (char*)message;
  _mqttLastWillRetain = retain;
}


// =============== Main loop / connection state handling =================

void EspMQTTClient::loop()
{
  // Delayed execution requests handling
  processDelayedExecutionRequests();

  // WIFI connection state handling

  bool isWifiConnected = (WiFi.status() == WL_CONNECTED);

  // A connection to wifi has just been established
  if (isWifiConnected && !_wifiConnected)
  {
    onWiFiConnectionEstablished();
    _lastWifiConnectionSuccessMillis = millis();
  }

  // The connection to wifi has just been lost
  else if (!isWifiConnected && _wifiConnected)
  {
    onWiFiConnectionLost();
  }

  // We are connected to wifi since at least one loop() call
  else if (isWifiConnected && _wifiConnected)
  {
    // Web updater handling
    if (_httpServer != NULL)
    {
      _httpServer->handleClient();
      #ifdef ESP8266
        MDNS.update(); // We need to do this only for ESP8266
      #endif
    }
  }

  // We are disconnected to wifi since at least one loop() call
  else
  {
    // We retry to connect to the wifi if we handle the reconnection to it 
    // and if there was no attempt since the last connection lost
    if (_wifiSsid != NULL && (_lastWifiConnectionAttemptMillis == 0 || _lastWifiConnectionSuccessMillis > _lastWifiConnectionAttemptMillis))
    {
      connectToWifi();
      _lastWifiConnectionAttemptMillis = millis();
    }
  }

  // If there is a change in the wifi connection state, don't handle the mqtt connection state right away.
  // This prevent the library from doing too much thing in the same loop() call.
  if (isWifiConnected != _wifiConnected)
  {
    _wifiConnected = isWifiConnected;
    return;
  }

  // MQTT Connection state handling

  bool isMqttConnected = isWifiConnected && _mqttClient.connected();
  
  // A connection to MQTT has just been established
  if (isMqttConnected && !_mqttConnected)
  {
    onMQTTConnectionEstablished();
  }

  // A connection to MQTT has just been lost
  else if (!isMqttConnected && _mqttConnected)
  {
    onMQTTConnectionLost();
  }

  // We are connected to MQTT since at least one loop() call
  else if (isMqttConnected && _mqttConnected)
  {
    _mqttClient.loop();
  }

  // We are not connected to MQTT since at least one loop() call, but we are connected to WIFI
  else if (isWifiConnected)
  {
    if (millis() - _lastMqttConnectionAttemptMillis > MQTT_CONNECTION_RETRY_DELAY || _lastMqttConnectionAttemptMillis == 0)
    {
      if (_enableMQTTConnect) {
		connectToMqttBroker();  
	  }
	  
	  
      _lastMqttConnectionAttemptMillis = millis();
    }
  }

  _mqttConnected = isMqttConnected;
}

void EspMQTTClient::onWiFiConnectionEstablished()
{
    if (_enableSerialLogs)
      Serial.printf("WiFi: Connected, ip : %s\n", WiFi.localIP().toString().c_str());

    // Config of web updater
    if (_httpServer != NULL)
    {
      MDNS.begin(_mqttClientName);
      _httpUpdater->setup(_httpServer, _updateServerAddress, _updateServerUsername, _updateServerPassword);
      _httpServer->begin();
      MDNS.addService("http", "tcp", 80);
    
      if (_enableSerialLogs)
        Serial.printf("WEB: Updater ready, open http://%s.local in your browser and login with username '%s' and password '%s'.\n", _mqttClientName, _updateServerUsername, _updateServerPassword);
    }
}

void EspMQTTClient::onWiFiConnectionLost()
{
  if (_enableSerialLogs)
    Serial.println("WiFi! Lost connection.");

  // If we handle wifi, we force disconnection to clear the last connection
  if (_wifiSsid != NULL)
    WiFi.disconnect();
}

void EspMQTTClient::onMQTTConnectionEstablished()
{
  _connectionEstablishedCount++;
  _connectionEstablishedCallback();
}

void EspMQTTClient::onMQTTConnectionLost()
{
  if (_enableSerialLogs)
    Serial.println("MQTT! Lost connection.");

  _topicSubscriptionListSize = 0;
}


// =============== Public functions for interaction with thus lib =================

bool EspMQTTClient::publish(const String &topic, const String &payload, bool retain)
{
  bool success = _mqttClient.publish(topic.c_str(), payload.c_str(), retain);

  if (_enableSerialLogs) 
  {
    if(success)
      Serial.printf("MQTT << [%s] %s\n", topic.c_str(), payload.c_str());
    else
      Serial.println("MQTT! publish failed, is the message too long ?"); // This can occurs if the message is too long according to the maximum defined in PubsubClient.h
  }

  return success;
}

bool EspMQTTClient::subscribe(const String &topic, MessageReceivedCallback messageReceivedCallback)
{
  // Check the possibility to add a new topic
  if (_topicSubscriptionListSize >= MAX_TOPIC_SUBSCRIPTION_LIST_SIZE) 
  {
    if (_enableSerialLogs)
      Serial.println("MQTT! Subscription list is full, ignored.");
    return false;
  }

  // Check the duplicate of the subscription to the topic
  bool found = false;
  for (byte i = 0; i < _topicSubscriptionListSize && !found; i++)
    found = _topicSubscriptionList[i].topic.equals(topic);

  if (found) 
  {
    if (_enableSerialLogs)
      Serial.printf("MQTT! Subscribed to [%s] already, ignored.\n", topic.c_str());
    return false;
  }

  // All checks are passed - do the job
  bool success = _mqttClient.subscribe(topic.c_str());

  if(success)
    _topicSubscriptionList[_topicSubscriptionListSize++] = { topic, messageReceivedCallback, NULL };
  
  if (_enableSerialLogs)
  {
    if(success)
      Serial.printf("MQTT: Subscribed to [%s]\n", topic.c_str());
    else
      Serial.println("MQTT! subscribe failed");
  }

  return success;
}

bool EspMQTTClient::subscribe(const String &topic, MessageReceivedCallbackWithTopic messageReceivedCallback)
{
  if(subscribe(topic, (MessageReceivedCallback)NULL))
  {
    _topicSubscriptionList[_topicSubscriptionListSize-1].callbackWithTopic = messageReceivedCallback;
    return true;
  }
  return false;
}

bool EspMQTTClient::unsubscribe(const String &topic)
{
  bool found = false;
  bool success = false;

  for (byte i = 0; i < _topicSubscriptionListSize; i++)
  {
    if (!found)
    {
      if (_topicSubscriptionList[i].topic.equals(topic))
      {
        found = true;
        success = _mqttClient.unsubscribe(topic.c_str());

        if (_enableSerialLogs)
        {
          if(success)
            Serial.printf("MQTT: Unsubscribed from %s\n", topic.c_str());
          else
            Serial.println("MQTT! unsubscribe failed");
        }
      }
    }

    if (found)
    {
      if ((i + 1) < MAX_TOPIC_SUBSCRIPTION_LIST_SIZE)
        _topicSubscriptionList[i] = _topicSubscriptionList[i + 1];
    }
  }

  if (found)
    _topicSubscriptionListSize--;
  else if (_enableSerialLogs)
    Serial.println("MQTT! Topic cannot be found to unsubscribe, ignored.");

  return success;
}

void EspMQTTClient::executeDelayed(const unsigned long delay, DelayedExecutionCallback callback)
{
  if (_delayedExecutionListSize < MAX_DELAYED_EXECUTION_LIST_SIZE)
  {
    DelayedExecutionRecord delayedExecutionRecord;
    delayedExecutionRecord.targetMillis = millis() + delay;
    delayedExecutionRecord.callback = callback;
    
    _delayedExecutionList[_delayedExecutionListSize] = delayedExecutionRecord;
    _delayedExecutionListSize++;
  }
  else if (_enableSerialLogs)
    Serial.printf("SYS! The list of delayed functions is full.\n");
}


// ================== Private functions ====================-

// Initiate a Wifi connection (non-blocking)
void EspMQTTClient::connectToWifi()
{
  WiFi.mode(WIFI_STA);
  #ifdef ESP32
    WiFi.setHostname(_mqttClientName);
  #else
    WiFi.hostname(_mqttClientName);
  #endif
  WiFi.begin(_wifiSsid, _wifiPassword);

  if (_enableSerialLogs)
    Serial.printf("\nWiFi: Connecting to %s ... \n", _wifiSsid);
}

// Try to connect to the MQTT broker and return True if the connection is successfull (blocking)
void EspMQTTClient::connectToMqttBroker()
{
//pj added April 26

if (_enableMACaddress) {
	uint8_t baseMac[6];
	esp_read_mac(baseMac, ESP_MAC_WIFI_STA);		// Get MAC address for WiFi station
	char baseMacChr[13] = {0};
	sprintf(baseMacChr, "%02X%02X%02X%02X%02X%02X", baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);       // Example: B4E62DA8EF6D
	const char*  _mqttClientName_PJ = (char*)baseMacChr;
	_mqttClientName =  _mqttClientName_PJ;
	
//end pj added April 26
}
if (_enableAuthentication) {	
//clientID = 
//username = public Key
//password = message signature
//message = clientID | ISO UTC time (16 digits)
// https://www.utctime.net/

  time_t now = time(nullptr);   //get current time
  time(&now);
  char ISOtime[sizeof "2011-10-08T07:07"];
  strftime(ISOtime, sizeof ISOtime, "%Y-%m-%dT%H:%M", gmtime(&now));
  Serial.println("ISO time");
  Serial.println(now);
  Serial.println(ISOtime);

  Serial.println("step1");

    char msg[400];
  Serial.println("step2");

    strcpy(msg,ISOtime);
  Serial.println("step3");
  Serial.println(msg);

char temp[20];
 //strcpy(temp,_mqttClientName);
 strcpy(temp,"B4E62DA8EF6D");

 
//const char*  temp = (char*)_mqttClientName;
//    Serial.println("step4");
//   Serial.println(temp);
//  delay(2);	 	
 
 
 
	strcat(msg,temp);
  Serial.println("step5");
  delay(2);	 	
 

  Serial.println("message");
  Serial.println(msg);

const char* ArkPublicKey = "03850f049eb4f13841ab805be51dfeed1b4e40ccadb6f82874dddcfd6cf58db325";       
static const auto PASSPHRASE        = "idle scrub portion party limb unit unveil wash tragic lyrics demand trick";  //TRXA2NUACckkYwWnS9JRkATQA453ukAcD1
//1stubby@
//AcjpFwKE5ixDMtsHaQysY7BVNWthCve7BV

      //sign the packet using Private Key
    Message messageToSign;
    messageToSign.sign(msg, PASSPHRASE);
    const auto signatureString = BytesToHex(messageToSign.signature);
	//_mqttPassword = (char*)signatureString.c_str();

	const char* _mqttPassword_PJauth;
	
	_mqttPassword_PJauth = signatureString.c_str();
	
	char passwordTemp[200];

	strcpy(passwordTemp,_mqttPassword_PJauth);

	_mqttUsername = ArkPublicKey;
	Serial.print("username: ");	
	Serial.println(ArkPublicKey);	

	
//	const char*  _mqttPassword_PJ = (char*)signatureString.c_str();
//	_mqttPassword = (char*)signatureString.c_str();


//strcpy(s2, s1.c_str());
	
	_mqttPassword = passwordTemp;
	
	//_mqttPassword = (char*)signatureString.c_str();
	Serial.print("password: ");	
	Serial.println(_mqttPassword);	
	
	//_mqttPassword
	
//	_mqttPassword =  "304402201ccc8c75501173afb99d7736b443910c68df2ee22d6cb19570f4cc4fbfcf52d502202f7947af03e49e8476ce61af9f0d1b3e52ef5f5afe9781483d77d04510919e9a";

//May 01 08:48:29 vultr.guest python3[2911]: [2020-05-01 08:48:29,331] :: DEBUG - <-in-- ConnectPacket(ts=2020-05-01 08:48:29.330486, fixed=MQTTFixedHeader(length=135, flags=0x0), variable=ConnectVariableHeader(proto_name=MQIsdp, proto_level=3, flags=0xc6, keepalive=15), payload=ConnectVariableHeader(client_id=2020-05-01T08:48B4E62DA8EF6D, will_topic=IOT/lastwill, will_message=b'Goodbye', username=03850f049eb4f13841ab805be51dfeed1b4e40ccadb6f82874dddcfd6cf58db325, password=))

 

 
 
	
 //     buf += F(",\"sig\":");
 //     buf += F("\"");
 //     buf += signatureString.c_str();   //append the signature

}
	
	
	
	
  if (_enableSerialLogs)
    Serial.printf("MQTT: Connecting to broker @%s with password  \"@%s\" ... ", _mqttServerIp, _mqttPassword);

  bool success = _mqttClient.connect(_mqttClientName, _mqttUsername, _mqttPassword, _mqttLastWillTopic, 0, _mqttLastWillRetain, _mqttLastWillMessage, _mqttCleanSession);

  if (_enableSerialLogs)
  {
    if (success) 
      Serial.println("ok.");
    else
    {
      Serial.print("unable to connect, reason: ");

      switch (_mqttClient.state())
      {
        case -4:
          Serial.println("MQTT_CONNECTION_TIMEOUT");
          break;
        case -3:
          Serial.println("MQTT_CONNECTION_LOST");
          break;
        case -2:
          Serial.println("MQTT_CONNECT_FAILED");
          break;
        case -1:
          Serial.println("MQTT_DISCONNECTED");
          break;
        case 1:
          Serial.println("MQTT_CONNECT_BAD_PROTOCOL");
          break;
        case 2:
          Serial.println("MQTT_CONNECT_BAD_CLIENT_ID");
          break;
        case 3:
          Serial.println("MQTT_CONNECT_UNAVAILABLE");
          break;
        case 4:
          Serial.println("MQTT_CONNECT_BAD_CREDENTIALS");
          break;
        case 5:
          Serial.println("MQTT_CONNECT_UNAUTHORIZED");
          break;
      }

      Serial.printf("MQTT: Retrying to connect in %i seconds.", MQTT_CONNECTION_RETRY_DELAY / 1000);
    }
  }
}

// Delayed execution handling. 
// Check if there is delayed execution requests to process and execute them if needed.
void EspMQTTClient::processDelayedExecutionRequests()
{
  if (_delayedExecutionListSize > 0)
  {
    unsigned long currentMillis = millis();

    for(byte i = 0 ; i < _delayedExecutionListSize ; i++)
    {
      if (_delayedExecutionList[i].targetMillis <= currentMillis)
      {
        _delayedExecutionList[i].callback();
        for(byte j = i ; j < _delayedExecutionListSize-1 ; j++)
          _delayedExecutionList[j] = _delayedExecutionList[j + 1];
        _delayedExecutionListSize--;
        i--;
      }
    }
  }
}

/**
 * Matching MQTT topics, handling the eventual presence of a single wildcard character
 *
 * @param topic1 is the topic may contain a wildcard
 * @param topic2 must not contain wildcards
 * @return true on MQTT topic match, false otherwise
 */
bool EspMQTTClient::mqttTopicMatch(const String &topic1, const String &topic2) 
{
  int i = 0;

  if((i = topic1.indexOf('#')) >= 0) 
  {
    String t1a = topic1.substring(0, i);
    String t1b = topic1.substring(i+1);
    if((t1a.length() == 0 || topic2.startsWith(t1a)) &&
       (t1b.length() == 0 || topic2.endsWith(t1b)))
      return true;
  } 
  else if((i = topic1.indexOf('+')) >= 0) 
  {
    String t1a = topic1.substring(0, i);
    String t1b = topic1.substring(i+1);

    if((t1a.length() == 0 || topic2.startsWith(t1a))&&
       (t1b.length() == 0 || topic2.endsWith(t1b))) 
    {
      if(topic2.substring(t1a.length(), topic2.length()-t1b.length()).indexOf('/') == -1)
        return true;
    }
  } 
  else 
  {
    return topic1.equals(topic2);
  }

  return false;
}

void EspMQTTClient::mqttMessageReceivedCallback(char* topic, byte* payload, unsigned int length)
{
  // Convert the payload into a String
  // First, We ensure that we dont bypass the maximum size of the PubSubClient library buffer that originated the payload
  // This buffer has a maximum length of MQTT_MAX_PACKET_SIZE and the payload begin at "headerSize + topicLength + 1"
  unsigned int strTerminationPos;
  if (strlen(topic) + length + 9 >= MQTT_MAX_PACKET_SIZE)
  {
    strTerminationPos = length - 1;

    if (_enableSerialLogs)
      Serial.print("MQTT! Your message may be truncated, please change MQTT_MAX_PACKET_SIZE of PubSubClient.h to a higher value.\n");
  }
  else
    strTerminationPos = length;

  // Second, we add the string termination code at the end of the payload and we convert it to a String object
  payload[strTerminationPos] = '\0';
  String payloadStr((char*)payload);
  String topicStr(topic);

  // Logging
  if (_enableSerialLogs)
    Serial.printf("MQTT >> [%s] %s\n", topic, payloadStr.c_str());

  // Send the message to subscribers
  for (byte i = 0 ; i < _topicSubscriptionListSize ; i++)
  {
    if (mqttTopicMatch(_topicSubscriptionList[i].topic, String(topic)))
    {
      if(_topicSubscriptionList[i].callback != NULL)
        _topicSubscriptionList[i].callback(payloadStr); // Call the callback
      if(_topicSubscriptionList[i].callbackWithTopic != NULL)
        _topicSubscriptionList[i].callbackWithTopic(topicStr, payloadStr); // Call the callback
    }
  }
}
