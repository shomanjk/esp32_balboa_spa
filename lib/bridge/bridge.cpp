#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <AsyncTCP.h>
#include <ArduinoLog.h>

#include  <spaUtilities.h>
#include <mqttModule.h>
#include <rs485.h>
#include <cacheRead.h>
#include <faultCapture.h>
#include <diagBridgeLog.h>

#include "bridge.h"
#include "spaMessage.h"
#include "balboa.h"

#define publishBridge(...) mqtt.publish((mqttTopic + "bridge/msg").c_str(), __VA_ARGS__)

// Local functions

void clientDataAvailable(void *r, AsyncClient *c, void *buf, size_t len);

CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> Q_out;

uint8_t message[BALBOA_MESSAGE_SIZE];

AsyncServer bridge(BALBOA_PORT);          // Port number for the server
AsyncClient *clients[MAX_BRIDGE_CLIENTS]; // Array to hold the clients
/** Last known IPv4 for each slot (`clients[i]`); AsyncTCP often reports `0.0.0.0` in teardown callbacks. */
static char bridgeClientRip[MAX_BRIDGE_CLIENTS][40];
WiFiUDP Discovery;

char packetBuffer[255]; // buffer to hold incoming packet
char replyBuffer[30];

bool bridgeStarted = false;
static uint32_t bridgeIngressSequence = 0;

static void bridgeStoreClientRip(int slot, AsyncClient *client)
{
  if (slot < 0 || slot >= MAX_BRIDGE_CLIENTS || client == nullptr)
  {
    return;
  }
  const String rip = client->remoteIP().toString();
  strncpy(bridgeClientRip[slot], rip.c_str(), sizeof(bridgeClientRip[slot]) - 1);
  bridgeClientRip[slot][sizeof(bridgeClientRip[slot]) - 1] = '\0';
}

static String bridgeRipForSlot(int slot, AsyncClient *c)
{
  if (slot >= 0 && slot < MAX_BRIDGE_CLIENTS && bridgeClientRip[slot][0] != '\0')
  {
    return String(bridgeClientRip[slot]);
  }
  return c != nullptr ? c->remoteIP().toString() : String("0.0.0.0");
}

/** Resolve stored IP when only `AsyncClient *` is available (e.g. ingress path). */
static String bridgeRipForClient(AsyncClient *c)
{
  if (c == nullptr)
  {
    return String("0.0.0.0");
  }
  for (int i = 0; i < MAX_BRIDGE_CLIENTS; i++)
  {
    if (clients[i] == c)
    {
      return bridgeRipForSlot(i, c);
    }
  }
  return c->remoteIP().toString();
}

static bool bridgeAnyClientConnected()
{
  for (int i = 0; i < MAX_BRIDGE_CLIENTS; i++)
  {
    if (clients[i] && clients[i]->connected())
    {
      return true;
    }
  }
  return false;
}

void bridgeSetup()
{
  String s = WiFi.macAddress();
  sprintf(replyBuffer, "BWGSPA\r\n00-15-27-%.2s-%.2s-%.2s\r\n", s.c_str() + 9, s.c_str() + 12, s.c_str() + 15);
  //  bridge.setNoDelay(true);
  Log.verbose(F("[Bridge]: Setup complete" CR));
};

void bridgeLoop()
{

  if (!bridgeStarted && WiFi.status() == WL_CONNECTED)
  {
    bridgeStarted = true;
    bridge.begin();
    Log.verbose(F("[Bridge]: Begin @ %p" CR), WiFi.localIP());
#ifdef LOCAL_CONNECT
    Discovery.begin(BALBOA_UDP_DISCOVERY_PORT);
#endif

    bridge.onClient([](void *arg, AsyncClient *client) { // On new client
#if defined(DIAG_FAULT_CAPTURE)
      {
        const String rip = client->remoteIP().toString();
        Log.warning(F("[Bridge]: New Client Connected %s" CR), rip.c_str());
        faultCaptureAppendf("[fault] bridge new_client ip=%s", rip.c_str());
      }
#else
      Log.verbose(F("[Bridge]: New Client Connected %s" CR), client->remoteIP().toString().c_str());
#endif
      bool added = false;
      for (int i = 0; i < MAX_BRIDGE_CLIENTS; i++)
      {
        if (clients[i] == nullptr || !clients[i]->connected())
        {
          clients[i] = client;
          bridgeStoreClientRip(i, client);
          const int slot = i;
          publishDebug(("Bridge Client Connected (" + String(i) + ") " + String(bridgeClientRip[slot])).c_str());
          added = true;
          clients[i]->onData(clientDataAvailable);
          clients[i]->onDisconnect([slot](void *r, AsyncClient *c) {
            const String rip = bridgeRipForSlot(slot, c);
#if defined(DIAG_FAULT_CAPTURE)
            Log.warning(F("[Bridge]: Disconnected from Spa %s" CR), rip.c_str());
            faultCaptureAppendf("[fault] bridge disconnect ip=%s", rip.c_str());
#else
            Log.verbose(F("[Bridge]: Disconnected from Spa %s" CR), rip.c_str());
#endif
            publishDebug(("Bridge Client Disconnected (" + String(slot) + ") " + rip).c_str());
            if (!bridgeAnyClientConnected())
            {
              Log.notice(F("[Bridge]: bridge/out idle — no TCP client connected" CR));
            }
          });
          clients[i]->onConnect([slot](void *r, AsyncClient *c) {
#if defined(DIAG_FAULT_CAPTURE)
            const String rip = bridgeRipForSlot(slot, c);
            Log.warning(F("[Bridge]: Connected to Spa %s" CR), rip.c_str());
            faultCaptureAppendf("[fault] bridge tcp_connect ip=%s", rip.c_str());
#else
            const String rip = bridgeRipForSlot(slot, c);
            Log.verbose(F("[Bridge]: Connected to Spa %s" CR), rip.c_str());
#endif
          });
          clients[i]->onTimeout([slot](void *r, AsyncClient *c, uint32_t time) {
#if defined(DIAG_FAULT_CAPTURE)
            const String rip = bridgeRipForSlot(slot, c);
            Log.warning(F("[Bridge]: Timeout from Spa %s ms=%lu" CR), rip.c_str(), static_cast<unsigned long>(time));
            faultCaptureAppendf("[fault] bridge timeout ip=%s", rip.c_str());
#else
            const String rip = bridgeRipForSlot(slot, c);
            Log.verbose(F("[Bridge]: Timeout from Spa %s ms=%lu" CR), rip.c_str(), static_cast<unsigned long>(time));
#endif
          });
          clients[i]->onError([slot](void *r, AsyncClient *c, int8_t error) {
#if defined(DIAG_FAULT_CAPTURE)
            const String rip = bridgeRipForSlot(slot, c);
            Log.warning(F("[Bridge]: Error from Spa %s err=%d" CR), rip.c_str(), static_cast<int>(error));
            faultCaptureAppendf("[fault] bridge tcp_error ip=%s err=%d", rip.c_str(), static_cast<int>(error));
#else
            const String rip = bridgeRipForSlot(slot, c);
            Log.verbose(F("[Bridge]: Error from Spa %s err=%d" CR), rip.c_str(), static_cast<int>(error));
#endif
          });
          break;
        }
      }

      if (!added)
      {
        publishError(("Bridge Client not Connected - no slots " + client->remoteIP().toString()).c_str());
#if defined(DIAG_FAULT_CAPTURE)
        {
          const String rip = client->remoteIP().toString();
          Log.warning(F("[Bridge]: no slots for client %s" CR), rip.c_str());
          faultCaptureAppendf("[fault] bridge no_slots ip=%s", rip.c_str());
        }
#endif
        client->close();
      }
    },
                    NULL); // We don't need to pass any arguments to the callback
  }
  /*
  // Handle data from connected clients
  for (int i = 0; i < MAX_BRIDGE_CLIENTS; i++)
  {
    if (clients[i]->connected())
    {
      if (!clients[i]->connected())
      {
        // Client disconnected
        publishDebug(("Client Disconnected (" + String(i) + ") " + clients[i]->remoteIP().toString()).c_str());
        clients[i]->stop();
      }
    }
  }*/

#ifdef LOCAL_CONNECT
  // Check for UDP Discovery
  int packetSize = Discovery.parsePacket();
  if (packetSize)
  {
    // receive incoming UDP packets
    Discovery.read(packetBuffer, 255);
    Discovery.beginPacket(Discovery.remoteIP(), Discovery.remotePort());
    Discovery.write((const uint8_t *)replyBuffer, strlen(replyBuffer));
    Discovery.endPacket();
    mqtt.publish((mqttTopic + "bridge/udpMsg").c_str(), packetBuffer);
  }
#endif
};

void clientDataAvailable(void *r, AsyncClient *client, void *buffer, size_t length)
{
  // Log.verbose(F("[Comm]: Data Available %x, %d" CR), String((char *)buf).substring(0, len).c_str(), len);

  if (length > 0 && length <= BALBOA_MESSAGE_SIZE)
  {
    u_int8_t *message = (u_int8_t *)buffer;
    const uint32_t ingressId = ++bridgeIngressSequence;
    const unsigned long ingressMs = millis();
    const String ingressStr = msgToString(message, length);
#if defined(DIAG_FAULT_CAPTURE)
    faultCaptureSetLastBridgeIngress(ingressStr.c_str());
#endif
    Log.verbose(F("[Bridge]: bridge/in %s" CR), ingressStr.c_str());
    BRIDGE_LOG_NOISY(F("[BridgeDiag]: ingress=%lu ms=%lu len=%u from=%s frame=%s" CR),
                     ingressId,
                     ingressMs,
                     static_cast<unsigned int>(length),
                     bridgeRipForClient(client).c_str(),
                     ingressStr.c_str());
    mqtt.publish((mqttTopic + "bridge/in").c_str(), ingressStr.c_str());
    if (length >= 5 && message[2] == id && message[4] == 0x04)
    {
      BRIDGE_LOG_NOISY(F("[BridgeDiag]: ingress=%lu handled=wifi_module_configuration" CR), ingressId);
      Q_out.clear();
      WiFi_Module_Configuration_Response(Q_out);
      bridgeSend(Q_out);
      Q_out.clear();
    }
    else
    {
      //  P_in.clear();
      publishBridge((String(length) + " Msg Received").c_str());
      BRIDGE_LOG_NOISY(F("[BridgeDiag]: ingress=%lu forwarding=cacheRead" CR), ingressId);
      cacheRead(message, length);
    }
  }
  else
  {
    publishBridge("No Msg Received");
  }
};

void bridgeSend(CircularBuffer<uint8_t, BALBOA_MESSAGE_SIZE> &data)
{
  bool sent = false;
  for (int i = 0; i < MAX_BRIDGE_CLIENTS; i++)
  {
    if (clients[i] && clients[i]->connected())
    {
      sent = true;

      const String ripOut = bridgeRipForSlot(i, clients[i]);
      Log.verbose(F("[Bridge]: before bridge/out %s - %s" CR), ripOut.c_str(), msgToString(data).c_str());
      uint8_t output[BALBOA_MESSAGE_SIZE];
      data.copyToArray(output);
      clients[i]->add(reinterpret_cast<const char *>(output), data.size());
      if (!clients[i]->send())
      {
        Log.error(F("[Bridge]: Error sending to client %s - %s" CR), ripOut.c_str(), msgToString(data).c_str());
#if defined(DIAG_FAULT_CAPTURE)
        faultCaptureAppendf("[fault] bridge send_fail ip=%s", ripOut.c_str());
#endif
      };

      // Log.verbose(F("[Bridge]: after bridge/out %p - %s" CR), clients[i]->remoteIP(), msgToString(data).c_str());
    }
    else
    {
    }
  }
  if (sent)
  {
    Log.verbose(F("[Bridge]: bridge/out %s" CR), msgToString(data).c_str());
    mqtt.publish((mqttTopic + "bridge/out").c_str(), msgToString(data).c_str());
  }
  data.clear();
};

void bridgeSend(uint8_t *message, int length)
{
  bool sent = false;
  for (int i = 0; i < MAX_BRIDGE_CLIENTS; i++)
  {
    if (clients[i] && clients[i]->connected())
    {
      sent = true;
      const String ripOut = bridgeRipForSlot(i, clients[i]);
      Log.verbose(F("[Bridge]: before bridge/out %s - %s" CR), ripOut.c_str(), msgToString(message, length).c_str());
      clients[i]->add(reinterpret_cast<const char *>(message), length);
      if (!clients[i]->send())
      {
        Log.error(F("[Bridge]: Error sending to client %s - %s" CR), ripOut.c_str(), msgToString(message, length).c_str());
#if defined(DIAG_FAULT_CAPTURE)
        faultCaptureAppendf("[fault] bridge send_fail ip=%s", ripOut.c_str());
#endif
      };
      // Log.verbose(F("[Bridge]: after bridge/out %p - %s" CR), clients[i]->remoteIP(), msgToString(message, length).c_str());
    }
  }
  if (sent)
  {
    // Log.verbose(F("[Bridge]: bridge/out %s" CR), msgToString(message, length).c_str());
    mqtt.publish((mqttTopic + "bridge/out").c_str(), msgToString(message, length).c_str());
  }
};