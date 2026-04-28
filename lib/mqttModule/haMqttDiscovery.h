#ifndef HA_MQTT_DISCOVERY_H
#define HA_MQTT_DISCOVERY_H

/** Minimal retained discovery on MQTT connect (no equipment until spa config is known). */
void publishHomeAssistantDiscovery();

/**
 * Publish or retract equipment / diagnostic discovery from spa configuration and information.
 * Call after configuration (0x2E) or information (0x24) responses when MQTT is up.
 * Uses Balboa convention: pump/light two-bit 0 = not installed; retracts removed slots for this power session.
 */
void publishHomeAssistantDiscoveryExpanded();

#endif
