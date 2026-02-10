/**
 * Media types supported by OpenClaw
 */
export enum MediaType {
  Image = 'image',
  Audio = 'audio',
  Video = 'video',
  Document = 'document',
  Voice = 'voice'
}

/**
 * Media capabilities that a channel can support
 */
export interface MediaCapabilities {
  sendImage?: boolean;
  receiveImage?: boolean;
  sendDocument?: boolean;
  receiveDocument?: boolean;
  sendAudio?: boolean;
  receiveAudio?: boolean;
  sendVideo?: boolean;
  receiveVideo?: boolean;
  sendVoice?: boolean;
  receiveVoice?: boolean;
}

/**
 * Channel capabilities
 */
export interface ChannelCapabilities {
  chatTypes: string[];
  media?: MediaCapabilities;
}

/**
 * Base context for channel operations
 */
export interface ChannelContext {
  channelId: string;
  peer: string;
}

/**
 * Context for sending text messages
 */
export interface ChannelSendTextContext extends ChannelContext {
  text: string;
}

/**
 * Context for sending media messages
 */
export interface ChannelSendMediaContext extends ChannelContext {
  text?: string; // Optional caption
  mediaPath?: string; // Local file path
  mediaUrl?: string; // Remote URL
  mediaType: MediaType;
  filename?: string;
  mimeType?: string;
}

/**
 * Inbound message from channel
 */
export interface InboundMessage {
  peer: string;
  text?: string; // Message text or caption
  mediaPath?: string; // Local path to downloaded media
  mediaUrl?: string; // URL to media
  mediaType?: MediaType;
  filename?: string;
  size?: number;
  timestamp: number;
  messageId: string;
  transcript?: string; // For audio/voice messages
}

/**
 * Channel plugin interface
 */
export interface ChannelPlugin {
  /**
   * Channel name
   */
  name: string;

  /**
   * Channel capabilities
   */
  capabilities: ChannelCapabilities;

  /**
   * Initialize the channel
   */
  initialize(): Promise<void>;

  /**
   * Send a text message
   */
  sendText(context: ChannelSendTextContext): Promise<void>;

  /**
   * Send a media message (optional, if media is supported)
   */
  sendMedia?(context: ChannelSendMediaContext): Promise<void>;

  /**
   * Handle inbound messages
   */
  onMessage(handler: (message: InboundMessage) => void): void;

  /**
   * Cleanup resources
   */
  cleanup(): Promise<void>;
}
