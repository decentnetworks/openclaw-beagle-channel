/**
 * OpenClaw Channel Plugin for Beagle Chat
 * 
 * This plugin enables OpenClaw agents to receive and send direct messages
 * through the Beagle Chat platform.
 */

import { createHmac } from 'crypto';
import type {
  BeagleChannelConfig,
  BeagleMessage,
  BeagleUser,
  BeagleWebhookPayload,
  BeagleSendMessageResponse,
} from './types.js';

/**
 * Message handler callback type
 */
type MessageHandler = (message: {
  userId: string;
  userName: string;
  text: string;
  messageId: string;
  timestamp: number;
}) => void | Promise<void>;

/**
 * BeagleChannel class - Main channel plugin implementation
 */
export class BeagleChannel {
  /**
   * Channel identifier
   */
  public readonly id = 'beagle';

  /**
   * Channel display label
   */
  public readonly label = 'Beagle Chat';

  /**
   * Channel configuration
   */
  private config?: BeagleChannelConfig;

  /**
   * Message handler callback
   */
  private messageHandler?: MessageHandler;

  /**
   * Debug logging enabled
   */
  private debug = false;

  /**
   * Initialize the channel with configuration
   */
  async initialize(config: BeagleChannelConfig): Promise<void> {
    this.config = config;
    this.debug = config.debug ?? false;

    this.log('Initializing Beagle channel...');
    
    // Validate required configuration
    if (!config.apiUrl) {
      throw new Error('Beagle API URL is required');
    }
    if (!config.authToken) {
      throw new Error('Beagle authentication token is required');
    }

    this.log('Beagle channel initialized successfully');
    this.log(`API URL: ${config.apiUrl}`);
  }

  /**
   * Register a message handler callback
   */
  onMessage(handler: MessageHandler): void {
    this.messageHandler = handler;
    this.log('Message handler registered');
  }

  /**
   * Send a direct message to a user
   */
  async sendMessage(userId: string, text: string): Promise<void> {
    if (!this.config) {
      throw new Error('Channel not initialized');
    }

    this.log(`Sending message to user ${userId}: ${text.substring(0, 50)}...`);

    try {
      const response = await fetch(`${this.config.apiUrl}/messages/send`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          'Authorization': `Bearer ${this.config.authToken}`,
        },
        body: JSON.stringify({
          to: userId,
          text: text,
          type: 'dm',
        }),
      });

      if (!response.ok) {
        const errorText = await response.text();
        throw new Error(`Failed to send message: ${response.status} ${errorText}`);
      }

      const result = await response.json() as BeagleSendMessageResponse;
      
      if (!result.success) {
        throw new Error(`Message sending failed: ${result.error}`);
      }

      this.log(`Message sent successfully: ${result.messageId}`);
    } catch (error) {
      const errorMessage = error instanceof Error ? error.message : 'Unknown error';
      this.log(`Error sending message: ${errorMessage}`);
      throw error;
    }
  }

  /**
   * Handle incoming webhook from Beagle
   */
  async handleWebhook(payload: BeagleWebhookPayload): Promise<void> {
    this.log(`Received webhook event: ${payload.event}`);

    // Verify webhook signature if secret is configured
    if (this.config?.webhookSecret && payload.signature) {
      const isValid = this.verifyWebhookSignature(payload);
      if (!isValid) {
        this.log('Invalid webhook signature, ignoring message');
        return;
      }
    }

    // Only process new messages
    if (payload.event !== 'message.created') {
      this.log(`Ignoring non-creation event: ${payload.event}`);
      return;
    }

    const message = payload.message;

    // Ignore messages without a sender
    if (!message.from) {
      this.log('Ignoring message without sender');
      return;
    }

    // Call the message handler if registered
    if (this.messageHandler) {
      this.log(`Processing message from ${message.from.name} (${message.from.id})`);
      
      try {
        await this.messageHandler({
          userId: message.from.id,
          userName: message.from.name,
          text: message.text,
          messageId: message.id,
          timestamp: message.timestamp,
        });
      } catch (error) {
        const errorMessage = error instanceof Error ? error.message : 'Unknown error';
        this.log(`Error handling message: ${errorMessage}`);
      }
    } else {
      this.log('No message handler registered, ignoring message');
    }
  }

  /**
   * Verify webhook signature using HMAC-SHA256
   */
  private verifyWebhookSignature(payload: BeagleWebhookPayload): boolean {
    if (!this.config?.webhookSecret || !payload.signature) {
      return false;
    }

    // Use HMAC-SHA256 for cryptographically secure signature verification
    const expectedSignature = this.generateSignature(JSON.stringify(payload.message));
    return payload.signature === expectedSignature;
  }

  /**
   * Generate HMAC-SHA256 signature for webhook verification
   */
  private generateSignature(data: string): string {
    if (!this.config?.webhookSecret) {
      return '';
    }
    
    // Use HMAC-SHA256 for cryptographically secure signature generation
    return createHmac('sha256', this.config.webhookSecret)
      .update(data)
      .digest('hex');
  }

  /**
   * Get user information from Beagle
   */
  async getUser(userId: string): Promise<BeagleUser | null> {
    if (!this.config) {
      throw new Error('Channel not initialized');
    }

    try {
      const response = await fetch(`${this.config.apiUrl}/users/${userId}`, {
        method: 'GET',
        headers: {
          'Authorization': `Bearer ${this.config.authToken}`,
        },
      });

      if (!response.ok) {
        if (response.status === 404) {
          return null;
        }
        throw new Error(`Failed to get user: ${response.status}`);
      }

      const user = await response.json() as BeagleUser;
      return user;
    } catch (error) {
      const errorMessage = error instanceof Error ? error.message : 'Unknown error';
      this.log(`Error getting user: ${errorMessage}`);
      return null;
    }
  }

  /**
   * Disconnect and cleanup
   */
  async disconnect(): Promise<void> {
    this.log('Disconnecting Beagle channel...');
    this.messageHandler = undefined;
    this.config = undefined;
    this.log('Beagle channel disconnected');
  }

  /**
   * Log debug messages
   */
  private log(message: string): void {
    if (this.debug) {
      console.log(`[BeagleChannel] ${message}`);
    }
  }
}

/**
 * Default export for OpenClaw plugin system
 */
export default BeagleChannel;

/**
 * Export all types
 */
export * from './types.js';
