"""Memory endpoints (BRIEFING section 2, signed off by Carlos 2026-10-15): view, add, edit and delete
everything the assistant remembers. Every change carries a call_id, so a retry never applies twice."""

from typing import Literal
from uuid import UUID

from fastapi import APIRouter, Depends, Request
from pydantic import BaseModel, Field, model_validator

from app.api import json_call
from app.auth import Caller, require_caller
from app.memory import MemoryEditor

router = APIRouter()


class MemoryAdd(BaseModel):
    call_id: UUID
    kind: Literal["contact", "preference", "fact", "routine"]
    label: str = Field(min_length=1, max_length=100)
    value: str = Field(min_length=1, max_length=300)


class MemoryEdit(BaseModel):
    call_id: UUID
    label: str | None = Field(default=None, min_length=1, max_length=100)
    value: str | None = Field(default=None, min_length=1, max_length=300)

    @model_validator(mode="after")
    def something_to_change(self):
        if self.label is None and self.value is None:
            raise ValueError("give a new label, a new value, or both")
        return self


class MemoryDelete(BaseModel):
    call_id: UUID


def _editor(request: Request) -> MemoryEditor:
    return MemoryEditor(request.app.state.services.lc.memory)


@router.get("/v1/memory")
async def list_memory(request: Request, caller: Caller = Depends(require_caller)):
    return await json_call(request, caller, "memory_list", None, lambda: _editor(request).list(caller.user_id))


@router.post("/v1/memory")
async def add_memory(body: MemoryAdd, request: Request, caller: Caller = Depends(require_caller)):
    return await json_call(request, caller, "memory_add", body,
                           lambda: _editor(request).add(caller.user_id, body.kind, body.label, body.value))


@router.patch("/v1/memory/{memory_id}")
async def edit_memory(memory_id: str, body: MemoryEdit, request: Request, caller: Caller = Depends(require_caller)):
    return await json_call(request, caller, "memory_edit", body,
                           lambda: _editor(request).edit(caller.user_id, memory_id, body.label, body.value))


@router.delete("/v1/memory/{memory_id}")
async def delete_memory(memory_id: str, body: MemoryDelete, request: Request, caller: Caller = Depends(require_caller)):
    return await json_call(request, caller, "memory_delete", body,
                           lambda: _editor(request).delete(caller.user_id, memory_id))
