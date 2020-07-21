// Generated by the protocol buffer compiler.  DO NOT EDIT!
// source: DunsAttribute.proto

package io.daos.dfs.uns;

public interface DunsAttributeOrBuilder extends
    // @@protoc_insertion_point(interface_extends:uns.DunsAttribute)
    com.google.protobuf.MessageOrBuilder {

  /**
   * <code>string puuid = 1;</code>
   * @return The puuid.
   */
  java.lang.String getPuuid();
  /**
   * <code>string puuid = 1;</code>
   * @return The bytes for puuid.
   */
  com.google.protobuf.ByteString
      getPuuidBytes();

  /**
   * <code>string cuuid = 2;</code>
   * @return The cuuid.
   */
  java.lang.String getCuuid();
  /**
   * <code>string cuuid = 2;</code>
   * @return The bytes for cuuid.
   */
  com.google.protobuf.ByteString
      getCuuidBytes();

  /**
   * <code>.uns.Layout layout_type = 3;</code>
   * @return The enum numeric value on the wire for layoutType.
   */
  int getLayoutTypeValue();
  /**
   * <code>.uns.Layout layout_type = 3;</code>
   * @return The layoutType.
   */
  io.daos.dfs.uns.Layout getLayoutType();

  /**
   * <code>string object_type = 4;</code>
   * @return The objectType.
   */
  java.lang.String getObjectType();
  /**
   * <code>string object_type = 4;</code>
   * @return The bytes for objectType.
   */
  com.google.protobuf.ByteString
      getObjectTypeBytes();

  /**
   * <code>uint64 chunk_size = 5;</code>
   * @return The chunkSize.
   */
  long getChunkSize();

  /**
   * <code>bool on_lustre = 6;</code>
   * @return The onLustre.
   */
  boolean getOnLustre();

  /**
   * <code>.uns.Properties properties = 7;</code>
   * @return Whether the properties field is set.
   */
  boolean hasProperties();
  /**
   * <code>.uns.Properties properties = 7;</code>
   * @return The properties.
   */
  io.daos.dfs.uns.Properties getProperties();
  /**
   * <code>.uns.Properties properties = 7;</code>
   */
  io.daos.dfs.uns.PropertiesOrBuilder getPropertiesOrBuilder();
}
