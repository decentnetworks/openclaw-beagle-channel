import axios, { AxiosInstance } from 'axios';
import {
  BeagleConfig,
  OutgoingTextMessage,
  OutgoingMediaMessage,
  SidecarTextRequest,
  SidecarMediaRequest,
} from './types';

/**
 * BeagleChannel handles outbound messages to Beagle Chat via sidecar
 */
export class BeagleChannel {
  private client: AxiosInstance;
  private config: BeagleConfig;

  constructor(config: BeagleConfig) {
    this.config = config;
    const baseURL = config.sidecarBaseUrl || 'http://127.0.0.1:39091';
    
    this.client = axios.create({
      baseURL,
      timeout: 30000,
      headers: {
        'Content-Type': 'application/json',
      },
    });
  }

  /**
   * Send a text message via the sidecar
   */
  async sendText(message: OutgoingTextMessage): Promise<void> {
    const request: SidecarTextRequest = {
      to: message.to,
      text: message.text,
      from: message.from,
      authToken: this.config.authToken,
    };

    try {
      await this.client.post('/sendText', request);
    } catch (error) {
      console.error('Failed to send text message:', error);
      throw error;
    }
  }

  /**
   * Send media (image, file, etc.) via the sidecar
   */
  async sendMedia(message: OutgoingMediaMessage): Promise<void> {
    const request: SidecarMediaRequest = {
      to: message.to,
      mediaUrl: message.mediaUrl,
      mediaPath: message.mediaPath,
      caption: message.caption,
      from: message.from,
      authToken: this.config.authToken,
    };

    try {
      await this.client.post('/sendMedia', request);
    } catch (error) {
      console.error('Failed to send media message:', error);
      throw error;
    }
  }
}
