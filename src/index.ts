import { OpenClawAPI, BeagleConfig } from './types';
import { BeagleChannel } from './channel';
import { startInboundService } from './service';
import { defaultConfig } from './config';

/**
 * Beagle Chat channel plugin for OpenClaw
 * 
 * This plugin enables OpenClaw to communicate with Beagle Chat (Elastos Carrier)
 * via a local sidecar service.
 */

export default {
  plugin: {
    name: 'beagle',
    displayName: 'Beagle Chat',
    description: 'Beagle Chat (Elastos Carrier) integration',
    version: '1.0.0',
  },
  
  /**
   * Setup function called by OpenClaw to register the plugin
   */
  setup(api: OpenClawAPI, config: BeagleConfig = {}) {
    // Merge user config with defaults
    const finalConfig: BeagleConfig = {
      ...defaultConfig,
      ...config,
    };

    console.log('Setting up Beagle Chat channel plugin');

    // Create and register the channel for outbound messages
    const channel = new BeagleChannel(finalConfig);
    api.registerChannel({ plugin: channel });

    // Register the inbound service for receiving messages
    api.registerService({ 
      start: async (serviceApi: OpenClawAPI, serviceConfig: BeagleConfig) => {
        await startInboundService(serviceApi, finalConfig);
      }
    });

    console.log('Beagle Chat channel plugin registered successfully');
  },
};

// Export types for external use
export * from './types';
export * from './config';
export { BeagleChannel } from './channel';
export { startInboundService } from './service';
