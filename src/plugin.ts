import {
  ChannelPlugin,
  ChannelCapabilities,
  ChannelSendTextContext,
  ChannelSendMediaContext,
  InboundMessage,
  MediaType
} from './types';
import { BeagleSidecar } from './sidecar-client';

/**
 * Beagle Channel Plugin for OpenClaw
 * 
 * Handles text and media messages for Beagle Chat via a sidecar API.
 */
export class BeagleChannelPlugin implements ChannelPlugin {
  name = 'beagle';
  
  capabilities: ChannelCapabilities = {
    chatTypes: ['direct'],
    media: {
      sendImage: true,
      receiveImage: true,
      sendDocument: true,
      receiveDocument: true,
      sendAudio: true,
      receiveAudio: true,
      sendVideo: true,
      receiveVideo: true,
      sendVoice: true,
      receiveVoice: true
    }
  };

  private sidecar: BeagleSidecar;
  private messageHandler?: (message: InboundMessage) => void;

  constructor(sidecarUrl: string = 'http://localhost:8080') {
    this.sidecar = new BeagleSidecar(sidecarUrl);
  }

  async initialize(): Promise<void> {
    // Connect to the sidecar WebSocket for inbound messages
    await this.sidecar.connect();
    
    // Listen for inbound messages from the sidecar
    this.sidecar.onMessage((message) => {
      if (this.messageHandler) {
        this.messageHandler(message);
      }
    });
  }

  async sendText(context: ChannelSendTextContext): Promise<void> {
    await this.sidecar.sendText({
      peer: context.peer,
      text: context.text
    });
  }

  async sendMedia(context: ChannelSendMediaContext): Promise<void> {
    // Validate that either mediaPath or mediaUrl is provided
    if (!context.mediaPath && !context.mediaUrl) {
      throw new Error('Either mediaPath or mediaUrl must be provided for media messages');
    }

    await this.sidecar.sendMedia({
      peer: context.peer,
      path: context.mediaPath,
      url: context.mediaUrl,
      mime: context.mimeType,
      filename: context.filename,
      caption: context.text
    });
  }

  onMessage(handler: (message: InboundMessage) => void): void {
    this.messageHandler = handler;
  }

  async cleanup(): Promise<void> {
    await this.sidecar.disconnect();
  }
}
